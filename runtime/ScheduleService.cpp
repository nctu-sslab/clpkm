/*
  ScheduleService.cpp

  Impl schedule service

*/

#include "ErrorHandling.hpp"
#include "ScheduleService.hpp"
#include <cstdlib>
#include <cstring>

using namespace CLPKM;



namespace {

// Watcher for low priority tasks
int RunLevelChangeWatcher(sd_bus_message* Msg, void* UserData,
                          sd_bus_error* ErrorRet) {

	(void) ErrorRet;

	auto& RunLevel = *reinterpret_cast<std::atomic<unsigned>*>(UserData);
	unsigned Level = 1;

	int Ret = sd_bus_message_read(Msg, "b", &Level);
	INTER_ASSERT(Ret >= 0,
	             "failed to read message from bus: %s", StrError(-Ret).c_str());

	RunLevel = Level;

	getRuntimeKeeper().Log(
			RuntimeKeeper::loglevel::INFO,
			"==CLPKM== Run level changed to %d\n",
			Level);

	return 1;

	}

} // namespace



// FIXME: change defaults to system bus
ScheduleService::ScheduleService()
: IsOnTerminate(false), IsOnSystemBus(false), Priority(priority::LOW),
  Bus(nullptr), Slot(nullptr), Threshold(0), RunLevel(0) {

	if (const char* Fine = getenv("CLPKM_PRIORITY")) {
		if (!strcmp(Fine, "high"))
			Priority = priority::HIGH;
		else if (strcmp(Fine, "low")) {
			getRuntimeKeeper().Log("==CLPKM== Unrecognised priority: \"%s\"\n",
			                       Fine);
			}
		}

	StartBus();

	}

ScheduleService::~ScheduleService() {
	this->Terminate();
	}



void ScheduleService::Terminate() {

	IsOnTerminate = true;

	if (Priority == priority::HIGH)
		CV.notify_all();

	if (IPCWorker.joinable())
		IPCWorker.join();

	// Clean up and reset to NULL
	Slot = sd_bus_slot_unref(Slot);
	Bus = sd_bus_flush_close_unref(Bus);

	}



void ScheduleService::StartBus() {

	int Ret = 0;

	if (IsOnSystemBus)
		Ret = sd_bus_open_system(&Bus);
	else
		Ret = sd_bus_open_user(&Bus);
	INTER_ASSERT(Ret >= 0, "failed to open bus: %s", StrError(-Ret).c_str());

	// Only low priority tasks need to get config
	if (Priority != priority::LOW) {
		IPCWorker = std::thread(&ScheduleService::HighPrioProcWorker, this);
		return;
		}

	Ret = sd_bus_add_match(
			Bus, &Slot,
			"type='signal',"
			"sender='edu.nctu.sslab.CLPKMSchedSrv',"
			"interface='edu.nctu.sslab.CLPKMSchedSrv',"
			"member='RunLevelChanged'",
			RunLevelChangeWatcher, &RunLevel);
	INTER_ASSERT(Ret >= 0, "failed to add match: %s", StrError(-Ret).c_str());

	sd_bus_error    BusError = SD_BUS_ERROR_NULL;
	sd_bus_message* Msg = nullptr;

	Ret = sd_bus_call_method(
			Bus,
			"edu.nctu.sslab.CLPKMSchedSrv",  // service
			"/edu/nctu/sslab/CLPKMSchedSrv", // object path
			"edu.nctu.sslab.CLPKMSchedSrv",  // interface
			"GetConfig",                     // method name
			&BusError, &Msg, "");
	INTER_ASSERT(Ret >= 0, "call method failed: %s", StrError(-Ret).c_str());

	const char* Path = nullptr;
	unsigned Level = 1;

	// Note: BOOLEAN uses one int to store its value
	//       Pass a C++ bool variable here can result in wild memory access
	Ret = sd_bus_message_read(Msg, "stb", &Path, &Threshold, &Level);
	INTER_ASSERT(Ret >= 0, "failed to read message: %s", StrError(-Ret).c_str());

	getRuntimeKeeper().Log(
		RuntimeKeeper::loglevel::INFO,
		"==CLPKM== Got config from the service:\n"
		"==CLPKM==   cc: \"%s\"\n"
		"==CLPKM==   threshold: %" PRIu64 "\n"
		"==CLPKM==   level: %d\n",
		Path, Threshold, Level);

	CompilerPath = Path;
	RunLevel = Level;

	sd_bus_error_free(&BusError);
	sd_bus_message_unref(Msg);

	IPCWorker = std::thread(&ScheduleService::LowPrioProcWorker, this);

	}



void ScheduleService::SchedStart() {
	if (Priority == priority::HIGH) {
		// Notify the worker that the RunLevel is no longer 0
		if (RunLevel++ == 0)
			CV.notify_one();
		return;
		}
	// Low priority task starts here
	std::unique_lock<std::mutex> Lock(Mutex);
	// Wait until RunLevel becomes 0
	CV.wait(Lock, [&]() -> bool {
		return !RunLevel;
		});
	}

void ScheduleService::SchedEnd() {
	if (Priority == priority::LOW)
		return;
	// If we reached here, the RunLevel is changed to 0, i.e. no running tasks
	// Notify the worker to inform the daemon
	if (--RunLevel == 0)
		CV.notify_one();
	}



// Workers
void ScheduleService::HighPrioProcWorker() {

	// Start from false because every process is initially low priority from the
	// perspective of the schedule service
	bool OldRunLevel = false;

	while (!IsOnTerminate) {

		std::unique_lock<std::mutex> Lock(Mutex);

		// Wait until the termination of this process, or the run level had
		// changed
		CV.wait(Lock, [&]() -> bool {
			bool RunLevelChanged = (OldRunLevel != static_cast<bool>(RunLevel));
			return IsOnTerminate || RunLevelChanged;
			});

		// If we got here due to IsOnTerminate
		if (IsOnTerminate)
			return;

		sd_bus_error    BusError = SD_BUS_ERROR_NULL;
		sd_bus_message* Msg = nullptr;

		// Note: bool is promoted to int here, no need to worry about the width
		int Ret = sd_bus_call_method(
				Bus,
				"edu.nctu.sslab.CLPKMSchedSrv",  // service
				"/edu/nctu/sslab/CLPKMSchedSrv", // object path
				"edu.nctu.sslab.CLPKMSchedSrv",  // interface
				"SetHighPrioProc",               // method name
				&BusError, &Msg, "b", (OldRunLevel = !OldRunLevel));
		INTER_ASSERT(Ret >= 0, "call method failed: %s", StrError(-Ret).c_str());

		unsigned IsGranted = 0;

		Ret = sd_bus_message_read(Msg, "b", &IsGranted);
		INTER_ASSERT(Ret >= 0,
		             "failed to read message: %s",
		             StrError(-Ret).c_str());

		INTER_ASSERT(IsGranted, "the schedule service denied priority change!");

		sd_bus_error_free(&BusError);
		sd_bus_message_unref(Msg);

		}

	}

void ScheduleService::LowPrioProcWorker() {

	// Start from the RunLevel set by the initial call to GetConfig
	bool OldRunLevel = static_cast<bool>(RunLevel);

	uint64_t Timeout = 0;

	{
		int Ret = sd_bus_get_timeout(Bus, &Timeout);
		INTER_ASSERT(Ret >= 0, "failed to get bus timeout", StrError(-Ret).c_str());

		// 0.1s shall be enough
		if (!Timeout)
			Timeout = 100000;

		getRuntimeKeeper().Log(
				RuntimeKeeper::loglevel::INFO,
				"==CLPKM== Timeout of sd_bus_wait is set to %" PRIu64 " us\n",
				Timeout);

		}

	while (!IsOnTerminate) {

		// This may change RunLevel
		int Ret = sd_bus_process(Bus, nullptr);
		INTER_ASSERT(Ret >= 0, "failed to process bus: %s", StrError(-Ret).c_str());

		if (Ret > 0)
			continue;

		// If we reach here, Ret is 0
		// No more stuff to process at the moment
		bool NewLevel = RunLevel;

		// If RunLevel changes from high to low, notify those waiting
		if (OldRunLevel != NewLevel) {
			if (OldRunLevel == static_cast<bool>(priority::HIGH))
				CV.notify_all();
			OldRunLevel = NewLevel;
			}

		// sd_bus_wait can only return on signal or timeout
		// We are running as a library, messing around signal seems not a good
		// idea
		Ret = sd_bus_wait(Bus, Timeout);
		INTER_ASSERT(Ret >= 0 || Ret == -EINTR,
		             "failed to wait on bus: %s", StrError(-Ret).c_str());

		}

	}



auto CLPKM::getScheduleService(void) -> ScheduleService& {
	static ScheduleService S;
	return S;
	}
