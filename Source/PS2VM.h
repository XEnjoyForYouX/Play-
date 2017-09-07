#pragma once

#include <thread>
#include <future>
#include "AppDef.h"
#include "Types.h"
#include "MIPS.h"
#include "MailBox.h"
#include "PadHandler.h"
#include "OpticalMedia.h"
#include "VirtualMachine.h"
#include "ee/Ee_SubSystem.h"
#include "iop/Iop_SubSystem.h"
#include "iop/IopBios.h"
#include "../tools/PsfPlayer/Source/SoundHandler.h"
#include "FrameDump.h"
#include "Profiler.h"

class CPS2VM : public CVirtualMachine
{
public:
	struct CPU_UTILISATION_INFO
	{
		int32 eeTotalTicks = 0;
		int32 eeIdleTicks = 0;

		int32 iopTotalTicks = 0;
		int32 iopIdleTicks = 0;
	};

	typedef std::unique_ptr<Ee::CSubSystem> EeSubSystemPtr;
	typedef std::unique_ptr<Iop::CSubSystem> IopSubSystemPtr;
	typedef std::function<void (const CFrameDump&)> FrameDumpCallback;
	typedef boost::signals2::signal<void (const CProfiler::ZoneArray&)> ProfileFrameDoneSignal;

								CPS2VM();
	virtual						~CPS2VM();

	void						Initialize();
	void						Destroy();

	void						StepEe();
	void						StepIop();
	void						StepVu0();
	void						StepVu1();

	void						Resume() override;
	void						Pause() override;
	void						Reset();

	STATUS						GetStatus() const override;

	void						DumpEEIntcHandlers();
	void						DumpEEDmacHandlers();

	void						CreateGSHandler(const CGSHandler::FactoryFunction&);
	CGSHandler*					GetGSHandler();
	void						DestroyGSHandler();

	void						CreatePadHandler(const CPadHandler::FactoryFunction&);
	CPadHandler*				GetPadHandler();
	void						DestroyPadHandler();

	void						CreateSoundHandler(const CSoundHandler::FactoryFunction&);
	void						DestroySoundHandler();

	std::future<bool>			SaveState(const boost::filesystem::path&);
	std::future<bool>			LoadState(const boost::filesystem::path&);

	void						TriggerFrameDump(const FrameDumpCallback&);

	CPU_UTILISATION_INFO		GetCpuUtilisationInfo() const;

#ifdef DEBUGGER_INCLUDED
	std::string					MakeDebugTagsPackagePath(const char*);
	void						LoadDebugTags(const char*);
	void						SaveDebugTags(const char*);
#endif

	CPadHandler*				m_pad;

	EeSubSystemPtr				m_ee;
	IopSubSystemPtr				m_iop;

	IopBiosPtr					m_iopOs;

	ProfileFrameDoneSignal		ProfileFrameDone;

private:
	typedef std::unique_ptr<COpticalMedia> OpticalMediaPtr;

	void						CreateVM();
	void						ResetVM();
	void						DestroyVM();
	bool						SaveVMState(const boost::filesystem::path&);
	bool						LoadVMState(const boost::filesystem::path&);

	void						ReloadExecutable(const char*, const CPS2OS::ArgumentList&);

	void						ResumeImpl();
	void						PauseImpl();
	void						DestroyImpl();
	
	void						CreateGsHandlerImpl(const CGSHandler::FactoryFunction&);
	void						DestroyGsHandlerImpl();

	void						CreatePadHandlerImpl(const CPadHandler::FactoryFunction&);
	void						DestroyPadHandlerImpl();
	
	void						CreateSoundHandlerImpl(const CSoundHandler::FactoryFunction&);
	void						DestroySoundHandlerImpl();

	void						UpdateEe();
	void						UpdateIop();
	void						UpdateSpu();

	void						OnGsNewFrame();

	void						CDROM0_Initialize();
	void						CDROM0_Mount(const char*);
	void						CDROM0_Reset();
	void						CDROM0_Destroy();
	void						SetIopOpticalMedia(COpticalMedia*);

	void						RegisterModulesInPadHandler();

	void						EmuThread();

	std::thread					m_thread;
	CMailBox					m_mailBox;
	STATUS						m_nStatus;
	bool						m_nEnd;

	int							m_vblankTicks = 0;
	bool						m_inVblank = 0;
	int							m_spuUpdateTicks = 0;
	int							m_eeExecutionTicks = 0;
	int							m_iopExecutionTicks = 0;

	CPU_UTILISATION_INFO		m_cpuUtilisation;

	bool						m_singleStepEe;
	bool						m_singleStepIop;
	bool						m_singleStepVu0;
	bool						m_singleStepVu1;

	CFrameDump					m_frameDump;
	FrameDumpCallback			m_frameDumpCallback;
	std::mutex					m_frameDumpCallbackMutex;
	bool						m_dumpingFrame = false;

	OpticalMediaPtr				m_cdrom0;

	enum
	{
		SAMPLE_COUNT = 44,
		BLOCK_SIZE = SAMPLE_COUNT * 2,
		BLOCK_COUNT = 400,
	};

	int16						m_samples[BLOCK_SIZE * BLOCK_COUNT];
	int							m_currentSpuBlock = 0;
	CSoundHandler*				m_soundHandler = nullptr;

	CProfiler::ZoneHandle		m_eeProfilerZone = 0;
	CProfiler::ZoneHandle		m_iopProfilerZone = 0;
	CProfiler::ZoneHandle		m_spuProfilerZone = 0;
	CProfiler::ZoneHandle		m_gsSyncProfilerZone = 0;
	CProfiler::ZoneHandle		m_otherProfilerZone = 0;
};
