#include <stdio.h>
#include <boost/lexical_cast.hpp>
#include <iomanip>
#include <functional>
#include "MainWindow.h"
#include "PtrMacro.h"
#include "../PS2VM.h"
#include "../PS2VM_Preferences.h"
#include "../PS2OS.h"
#include "../AppConfig.h"
#include "GSH_OpenGLWin32.h"
#include "../GSH_Null.h"
#include "PH_DirectInput.h"
#include "win32/FileDialog.h"
#include "win32/AcceleratorTableGenerator.h"
#include "VFSManagerWnd.h"
#include "McManagerWnd.h"
#include "Debugger.h"
#include "SysInfoWnd.h"
#include "AboutWnd.h"
#include "../Profiler.h"
#include "resource.h"
#include "string_cast.h"
#include "FileFilters.h"

#define CLSNAME						_T("MainWindow")
#define WNDSTYLE					(WS_CLIPCHILDREN | WS_DLGFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX)

#define STATUSPANEL					0
#define FPSPANEL					1

#define FILEMENUPOS					0
#define VMMENUPOS					1

#define ID_MAIN_VM_STATESLOT_0		(0xBEEF)
#define MAX_STATESLOTS				10

#define ID_MAIN_DEBUG_SHOW			(0xDEAD)

#define PREF_UI_PAUSEWHENFOCUSLOST	"ui.pausewhenfocuslost"

double CMainWindow::m_nStatusBarPanelWidths[2] =
{
	0.7,
	0.3,
};

CMainWindow::CMainWindow(CPS2VM& virtualMachine, char* sCmdLine) 
: m_virtualMachine(virtualMachine)
, m_recordingAvi(false)
, m_recordBuffer(NULL)
, m_recordBufferWidth(0)
, m_recordBufferHeight(0)
, m_recordAviMutex(NULL)
, m_nFrames(0)
, m_drawCallCount(0)
, m_nStateSlot(0)
{
	m_recordAviMutex = CreateMutex(NULL, FALSE, NULL);

	RECT rc;
	TCHAR sVersion[256];

	CAppConfig::GetInstance().RegisterPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST, true);

	if(!DoesWindowClassExist(CLSNAME))
	{
		WNDCLASSEX wc;
		memset(&wc, 0, sizeof(WNDCLASSEX));
		wc.cbSize			= sizeof(WNDCLASSEX);
		wc.hCursor			= LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground	= (HBRUSH)(COLOR_WINDOW); 
		wc.hInstance		= GetModuleHandle(NULL);
		wc.lpszClassName	= CLSNAME;
		wc.lpfnWndProc		= CWindow::WndProc;
		wc.style			= CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
		RegisterClassEx(&wc);
	}

	SetRect(&rc, 0, 0, 640, 480);
	
	Create(NULL, CLSNAME, _T(""), WNDSTYLE, &rc, NULL, NULL);
	SetClassPtr();

#ifdef DEBUGGER_INCLUDED
	CDebugger::InitializeConsole();
#endif

	m_virtualMachine.Initialize();

	SetIcon(ICON_SMALL, LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_PUREI)));
	SetIcon(ICON_BIG, LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(IDI_PUREI)));

	SetMenu(LoadMenu(GetModuleHandle(NULL), MAKEINTRESOURCE(IDR_MAINWINDOW)));

#ifdef DEBUGGER_INCLUDED
	m_pDebugger = new CDebugger(m_virtualMachine);
	CreateDebugMenu();
#endif

	PrintVersion(sVersion, countof(sVersion));

	SetRect(&rc, 0, 0, 1, 1);
	m_pOutputWnd = new COutputWnd(m_hWnd, &rc);

	m_pStatusBar = new Framework::Win32::CStatusBar(m_hWnd);
	m_pStatusBar->SetParts(2, m_nStatusBarPanelWidths);
	m_pStatusBar->SetText(STATUSPANEL,	sVersion);
	m_pStatusBar->SetText(FPSPANEL,		_T(""));

	m_pOutputWnd->m_OnSizeChange.connect(boost::bind(&CMainWindow::OnOutputWndSizeChange, this));

	//m_virtualMachine.CreateGSHandler(CGSH_Null::GetFactoryFunction());
	m_virtualMachine.CreateGSHandler(CGSH_OpenGLWin32::GetFactoryFunction(m_pOutputWnd));

	m_virtualMachine.CreatePadHandler(CPH_DirectInput::GetFactoryFunction(m_hWnd));

	m_nDeactivatePause = false;
	m_nPauseFocusLost = CAppConfig::GetInstance().GetPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST);

	m_virtualMachine.m_pGS->OnNewFrame.connect(boost::bind(&CMainWindow::OnNewFrame, this, _1));

	SetTimer(m_hWnd, NULL, 1000, NULL);
	//Initialize status bar
	OnTimer(0);

	m_virtualMachine.m_os->OnExecutableChange.connect(boost::bind(&CMainWindow::OnExecutableChange, this));

	CreateStateSlotMenu();
	CreateAccelerators();

	if(strstr(sCmdLine, "-cdrom0") != NULL)
	{
		BootCDROM();
	}
	else if(strlen(sCmdLine))
	{
		LoadELF(sCmdLine);
	}

	UpdateUI();
	Show(SW_SHOW);

#if (_DEBUG && DEBUGGER_INCLUDED)
	ShowDebugger();
#endif
}

CMainWindow::~CMainWindow()
{
	m_virtualMachine.Pause();

	m_virtualMachine.DestroyPadHandler();
	m_virtualMachine.DestroyGSHandler();

#ifdef DEBUGGER_INCLUDED
	DELETEPTR(m_pDebugger);
#endif

	DELETEPTR(m_pOutputWnd);
	DELETEPTR(m_pStatusBar);

	DestroyAcceleratorTable(m_nAccTable);

	if(m_recordingAvi)
	{
		m_aviStream.Close();
		m_recordingAvi = false;
	}

	CloseHandle(m_recordAviMutex);
	m_recordAviMutex = NULL;

	m_virtualMachine.Destroy();
}

int CMainWindow::Loop()
{
	while(IsWindow())
	{
		MSG msg;
		GetMessage(&msg, NULL, 0, 0);
		bool nDispatched = false;
		HWND hActive = GetActiveWindow();

		if(hActive == m_hWnd)
		{
			nDispatched = TranslateAccelerator(m_hWnd, m_nAccTable, &msg) != 0;
		}
#ifdef DEBUGGER_INCLUDED
		else if(hActive == m_pDebugger->m_hWnd)
		{
			nDispatched = TranslateAccelerator(m_pDebugger->m_hWnd, m_pDebugger->GetAccelerators(), &msg) != 0;
		}
#endif
		if(!nDispatched)
		{
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
	}
	return 0;
}

long CMainWindow::OnCommand(unsigned short nID, unsigned short nCmd, HWND hSender)
{
	switch(nID)
	{
	case ID_MAIN_FILE_LOADELF:
		OpenELF();
		break;
	case ID_MAIN_FILE_BOOTCDROM:
		BootCDROM();
		break;
	case ID_MAIN_FILE_BOOTDISKIMAGE:
		BootDiskImage();
		break;
	case ID_MAIN_FILE_RECORDAVI:
		RecordAvi();
		break;
	case ID_MAIN_FILE_EXIT:
		DestroyWindow(m_hWnd);
		break;
	case ID_MAIN_VM_RESUME:
		ResumePause();
		break;
	case ID_MAIN_VM_RESET:
		Reset();
		break;
	case ID_MAIN_VM_PAUSEFOCUS:
		PauseWhenFocusLost();
		break;
	case ID_MAIN_VM_SAVESTATE:
		SaveState();
		break;
	case ID_MAIN_VM_LOADSTATE:
		LoadState();
		break;
	case ID_MAIN_VM_FRAMESKIP_DECREMENT:
		ChangeFrameskip(true);
		break;
	case ID_MAIN_VM_FRAMESKIP_INCREMENT:
		ChangeFrameskip(false);
		break;
	case ID_MAIN_VM_STATESLOT_0 + 0:
	case ID_MAIN_VM_STATESLOT_0 + 1:
	case ID_MAIN_VM_STATESLOT_0 + 2:
	case ID_MAIN_VM_STATESLOT_0 + 3:
	case ID_MAIN_VM_STATESLOT_0 + 4:
	case ID_MAIN_VM_STATESLOT_0 + 5:
	case ID_MAIN_VM_STATESLOT_0 + 6:
	case ID_MAIN_VM_STATESLOT_0 + 7:
	case ID_MAIN_VM_STATESLOT_0 + 8:
	case ID_MAIN_VM_STATESLOT_0 + 9:
		ChangeStateSlot(nID - ID_MAIN_VM_STATESLOT_0);
		break;
	case ID_MAIN_OPTIONS_RENDERER:
		ShowRendererSettings();
		break;
	case ID_MAIN_OPTIONS_CONTROLLER:
		ShowControllerSettings();
		break;
	case ID_MAIN_OPTIONS_VFSMANAGER:
		ShowVfsManager();
		break;
	case ID_MAIN_OPTIONS_MCMANAGER:
		ShowMcManager();
		break;
	case ID_MAIN_DEBUG_SHOW:
		ShowDebugger();
		break;
	case ID_MAIN_HELP_SYSINFO:
		ShowSysInfo();
		break;
	case ID_MAIN_HELP_ABOUT:
		ShowAbout();
		break;

	}
	return TRUE;
}

long CMainWindow::OnTimer(WPARAM)
{
	uint32 dcpf = (m_nFrames != 0) ? (m_drawCallCount / m_nFrames) : 0;
	std::tstring sCaption = boost::lexical_cast<std::tstring>(m_nFrames) + _T(" f/s, ")
		+ boost::lexical_cast<std::tstring>(dcpf) + _T(" dc/f");
	m_pStatusBar->SetText(FPSPANEL, sCaption.c_str());

	m_nFrames = 0;
	m_drawCallCount = 0;

#ifdef PROFILE

	CProfiler::ZoneList Zones;
	double nTotalTime;
	xstringstream sProfileCaption;

	Zones = CProfiler::GetInstance().GetStats();
	CProfiler::GetInstance().Reset();

	nTotalTime = 0;

	for(CProfiler::ZoneList::iterator itZone = Zones.begin();
		itZone != Zones.end();
		itZone++)
	{
		nTotalTime += (*itZone).GetTime();		
	}

	sProfileCaption.precision(4);

	for(CProfiler::ZoneList::iterator itZone = Zones.begin();
		itZone != Zones.end();
		itZone++)
	{
		sProfileCaption << (*itZone).GetName() << ": " << setw(5) << ((double)(*itZone).GetTime() / nTotalTime) * 100 << " ";
	}

	m_pStatusBar->SetText(STATUSPANEL, sProfileCaption.str().c_str());


#endif

	return TRUE;
}

long CMainWindow::OnActivateApp(bool nActive, unsigned long nThreadId)
{
	if(m_nPauseFocusLost == true)
	{
		if(nActive == false)
		{
			if(m_virtualMachine.GetStatus() == CVirtualMachine::RUNNING)
			{
				ResumePause();
				m_nDeactivatePause = true;
			}
		}
		
		if((nActive == true) && (m_nDeactivatePause == true))
		{
			ResumePause();
			m_nDeactivatePause = false;
		}
	}
	return FALSE;
}

void CMainWindow::OpenELF()
{
	Framework::Win32::CFileDialog d;
	d.m_OFN.lpstrFilter = _T("ELF Executable Files (*.elf)\0*.elf\0All files (*.*)\0*.*\0");

	Enable(FALSE);
	int nRet = d.SummonOpen(m_hWnd);
	Enable(TRUE);
	SetFocus();

	if(nRet == 0) return;

	LoadELF(string_cast<std::string>(d.GetPath()).c_str());
}

void CMainWindow::ResumePause()
{
	if(m_virtualMachine.GetStatus() == CVirtualMachine::RUNNING)
	{
		m_virtualMachine.Pause();
		SetStatusText(_T("Virtual Machine paused."));
	}
	else
	{
		m_virtualMachine.Resume();
		SetStatusText(_T("Virtual Machine resumed."));
	}
}

void CMainWindow::Reset()
{
	if(m_lastOpenCommand)
	{
		m_lastOpenCommand->Execute(this);
	}
}

void CMainWindow::PauseWhenFocusLost()
{
	m_nPauseFocusLost = !m_nPauseFocusLost;
	if(m_nPauseFocusLost)
	{
		m_nDeactivatePause = false;
	}

	CAppConfig::GetInstance().SetPreferenceBoolean(PREF_UI_PAUSEWHENFOCUSLOST, m_nPauseFocusLost);
	UpdateUI();
}

void CMainWindow::SaveState()
{
	if(m_virtualMachine.m_os->GetELF() == NULL) return;

	if(m_virtualMachine.SaveState(GenerateStatePath().c_str()) == 0)
	{
		PrintStatusTextA("Saved state to slot %i.", m_nStateSlot);
	}
	else
	{
		PrintStatusTextA("Error saving state to slot %i.", m_nStateSlot);
	}
}

void CMainWindow::LoadState()
{
	if(m_virtualMachine.m_os->GetELF() == NULL) return;

	if(m_virtualMachine.LoadState(GenerateStatePath().c_str()) == 0)
	{
		PrintStatusTextA("Loaded state from slot %i.", m_nStateSlot);
	}
	else
	{
		PrintStatusTextA("Error loading state from slot %i.", m_nStateSlot);
	}
}

void CMainWindow::ChangeFrameskip(bool decrement)
{
	int frameSkip = CAppConfig::GetInstance().GetPreferenceInteger(PREF_PS2_FRAMESKIP);
	frameSkip += decrement ? -1 : 1;
	frameSkip = std::max<int>(frameSkip, 0);
	frameSkip = std::min<int>(frameSkip, 10);
	m_virtualMachine.SetFrameSkip(frameSkip);
	PrintStatusTextA("Frame skip set to %i.", frameSkip);
}

void CMainWindow::ChangeStateSlot(unsigned int nSlot)
{
	m_nStateSlot = nSlot % MAX_STATESLOTS;
	UpdateUI();
}

void CMainWindow::ShowDebugger()
{
#ifdef DEBUGGER_INCLUDED
	m_pDebugger->Show(SW_MAXIMIZE);
	SetForegroundWindow(m_pDebugger->m_hWnd);
#endif
}

void CMainWindow::ShowSysInfo()
{
	{
		CSysInfoWnd SysInfoWnd(m_hWnd);
		SysInfoWnd.DoModal();
	}
	Redraw();
}

void CMainWindow::ShowAbout()
{
	{
		CAboutWnd AboutWnd(m_hWnd);
		AboutWnd.DoModal();
	}
	Redraw();
}

void CMainWindow::ShowSettingsDialog(CSettingsDialogProvider* provider)
{
	if(!provider) return;

	CScopedVmPauser vmPauser(m_virtualMachine);

	Framework::Win32::CModalWindow* pWindow = provider->CreateSettingsDialog(m_hWnd);
	pWindow->DoModal();
	DELETEPTR(pWindow);
	provider->OnSettingsDialogDestroyed();

	Redraw();
}

void CMainWindow::ShowRendererSettings()
{
	ShowSettingsDialog(dynamic_cast<CSettingsDialogProvider*>(m_virtualMachine.GetGSHandler()));
}

void CMainWindow::ShowControllerSettings()
{
	if(!m_virtualMachine.m_pPad) return;
	ShowSettingsDialog(dynamic_cast<CSettingsDialogProvider*>(m_virtualMachine.m_pPad));
}

void CMainWindow::ShowVfsManager()
{
	bool nPaused = false;

	if(m_virtualMachine.GetStatus() == CVirtualMachine::RUNNING)
	{
		nPaused = true;
		ResumePause();
	}

	CVFSManagerWnd VFSManagerWnd(m_hWnd);
	VFSManagerWnd.DoModal();

	Redraw();

	if(nPaused)
	{
		ResumePause();
	}
}

void CMainWindow::ShowMcManager()
{
	bool nPaused = false;

	if(m_virtualMachine.GetStatus() == CVirtualMachine::RUNNING)
	{
		nPaused = true;
		ResumePause();
	}

	CMcManagerWnd McManagerWnd(m_hWnd);
	McManagerWnd.DoModal();

	Redraw();

	if(nPaused)
	{
		ResumePause();
	}	
}

void CMainWindow::LoadELF(const char* sFilename)
{
	CPS2OS& os = *m_virtualMachine.m_os;
	m_virtualMachine.Pause();
	m_virtualMachine.Reset();

	try
	{
		os.BootFromFile(sFilename);
#if !defined(_DEBUG) && !defined(DEBUGGER_INCLUDED)
		m_virtualMachine.Resume();
#endif
		m_lastOpenCommand = OpenCommandPtr(new CLoadElfOpenCommand(sFilename));
		PrintStatusTextA("Loaded executable '%s'.", os.GetExecutableName());
	}
	catch(const std::exception& Exception)
	{
		MessageBoxA(m_hWnd, Exception.what(), NULL, 16);
	}
}

void CMainWindow::BootCDROM()
{
	CPS2OS& os = *m_virtualMachine.m_os;
	m_virtualMachine.Pause();
	m_virtualMachine.Reset();

	try
	{
		os.BootFromCDROM(CPS2OS::ArgumentList());
#ifndef _DEBUG
		m_virtualMachine.Resume();
#endif
		m_lastOpenCommand = OpenCommandPtr(new CBootCdRomOpenCommand());
		PrintStatusTextA("Loaded executable '%s' from cdrom0.", os.GetExecutableName());
	}
	catch(const std::exception& Exception)
	{
		MessageBoxA(m_hWnd, Exception.what(), NULL, 16);
	}
}

void CMainWindow::BootDiskImage()
{
	Framework::Win32::CFileDialog d;
	d.m_OFN.lpstrFilter = DISKIMAGE_FILTER;

	Enable(FALSE);
	int nRet = d.SummonOpen(m_hWnd);
	Enable(TRUE);
	SetFocus();

	if(nRet == 0) return;

	CAppConfig::GetInstance().SetPreferenceString(PS2VM_CDROM0PATH, string_cast<std::string>(d.GetPath()).c_str());
	BootCDROM();
}

void CMainWindow::RecordAvi()
{
	if(m_recordingAvi)
	{
		m_recordingAvi = false;

		while(1)
		{
			DWORD result = MsgWaitForMultipleObjects(1, &m_recordAviMutex, FALSE, INFINITE, QS_ALLINPUT);
			if(result == WAIT_OBJECT_0) break;
			MSG msg;
			GetMessage(&msg, NULL, 0, 0);
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		
		{
			m_aviStream.Close();
			delete [] m_recordBuffer;
			m_recordBuffer = NULL;
			m_recordBufferWidth = 0;
			m_recordBufferHeight = 0;
		}
		ReleaseMutex(m_recordAviMutex);

		UpdateUI();
	}
	else
	{
		CScopedVmPauser vmPauser(m_virtualMachine);

		Framework::Win32::CFileDialog d;
		d.m_OFN.lpstrFilter = _T("AVI Movie Files (*.avi)\0*.avi\0All files (*.*)\0*.*\0");
		if(!d.SummonSave(m_hWnd))
		{
			return;
		}

		RECT rc;
		m_pOutputWnd->GetWindowRect(&rc);
		unsigned int nViewW = rc.right - rc.left;
		unsigned int nViewH = rc.bottom - rc.top;

		m_aviStream.SetSize(nViewW, nViewH);
		if(!m_aviStream.Open(m_hWnd, d.GetPath()))
		{
			MessageBox(m_hWnd, _T("Failed to start AVI recording."), APP_NAME, 16);
			return;
		}

		m_recordBufferWidth = nViewW;
		m_recordBufferHeight = nViewH;
		m_recordBuffer = new uint8[m_recordBufferWidth * m_recordBufferHeight * 4];

		m_recordingAvi = true;
		UpdateUI();
	}
}

void CMainWindow::RefreshLayout()
{
	RECT rc;
	m_pOutputWnd->GetWindowRect(&rc);

	unsigned int nViewW = rc.right - rc.left;
	unsigned int nViewH = rc.bottom - rc.top;

	SetRect(&rc, 0, 0, nViewW, nViewH);
	rc.bottom += m_pStatusBar->GetHeight();

	AdjustWindowRect(&rc, WNDSTYLE, TRUE);

	SetSize(rc.right - rc.left, rc.bottom - rc.top);

	m_pStatusBar->RefreshGeometry();
	m_pStatusBar->SetParts(2, m_nStatusBarPanelWidths);

	//m_pStatusBar->SetPosition(0, nViewH + 2);
	//m_pStatusBar->SetSize(nViewW, 21);

	Center();
}

void CMainWindow::PrintStatusTextA(const char* sFormat, ...)
{
	char sText[256];
	va_list Args;

	va_start(Args, sFormat);
	_vsnprintf(sText, 256, sFormat, Args);
	va_end(Args);

	m_pStatusBar->SetText(STATUSPANEL, string_cast<std::tstring>(sText).c_str());
}

void CMainWindow::SetStatusText(const TCHAR* sText)
{
	m_pStatusBar->SetText(STATUSPANEL, sText);
}

void CMainWindow::CreateAccelerators()
{
	Framework::Win32::CAcceleratorTableGenerator generator;
	generator.Insert(ID_MAIN_VM_RESUME,                 VK_F5,          FVIRTKEY);
	generator.Insert(ID_MAIN_FILE_LOADELF,              'O',            FVIRTKEY | FCONTROL);
	generator.Insert(ID_MAIN_VM_SAVESTATE,              VK_F7,          FVIRTKEY);
	generator.Insert(ID_MAIN_VM_LOADSTATE,              VK_F8,          FVIRTKEY);
	generator.Insert(ID_MAIN_VM_FRAMESKIP_DECREMENT,    VK_SUBTRACT,    FVIRTKEY);
	generator.Insert(ID_MAIN_VM_FRAMESKIP_INCREMENT,    VK_ADD,         FVIRTKEY);
	m_nAccTable = generator.Create();
}

void CMainWindow::CreateDebugMenu()
{
	HMENU hMenu = CreatePopupMenu();
	InsertMenu(hMenu, 0, MF_STRING, ID_MAIN_DEBUG_SHOW, _T("Show Debugger"));

	MENUITEMINFO ItemInfo;
	memset(&ItemInfo, 0, sizeof(MENUITEMINFO));
	ItemInfo.cbSize		= sizeof(MENUITEMINFO);
	ItemInfo.fMask		= MIIM_STRING | MIIM_SUBMENU;
	ItemInfo.dwTypeData	= _T("Debug");
	ItemInfo.hSubMenu	= hMenu;

	InsertMenuItem(GetMenu(m_hWnd), 3, TRUE, &ItemInfo);
}

void CMainWindow::CreateStateSlotMenu()
{
	HMENU hMenu = CreatePopupMenu();
	for(unsigned int i = 0; i < MAX_STATESLOTS; i++)
	{
		std::tstring sCaption = _T("Slot ") + boost::lexical_cast<std::tstring>(i);
		InsertMenu(hMenu, i, MF_STRING, ID_MAIN_VM_STATESLOT_0 + i, sCaption.c_str());
	}

	MENUITEMINFO ItemInfo;
	memset(&ItemInfo, 0, sizeof(MENUITEMINFO));
	ItemInfo.cbSize		= sizeof(MENUITEMINFO);
	ItemInfo.fMask		= MIIM_SUBMENU;
	ItemInfo.hSubMenu	= hMenu;

	hMenu = GetSubMenu(GetMenu(m_hWnd), VMMENUPOS);
	SetMenuItemInfo(hMenu, ID_MAIN_VM_STATESLOT, FALSE, &ItemInfo);
}

std::string CMainWindow::GenerateStatePath()
{
	return std::string("./states/") + 
		m_virtualMachine.m_os->GetExecutableName() + ".st" + 
		boost::lexical_cast<std::string>(m_nStateSlot) + ".zip";
}

void CMainWindow::UpdateUI()
{
	CPS2OS& os = *m_virtualMachine.m_os;

	//Fix the file menu
	{
		HMENU hMenu = GetSubMenu(GetMenu(m_hWnd), FILEMENUPOS);

		ModifyMenu(hMenu, ID_MAIN_FILE_RECORDAVI, MF_BYCOMMAND | MF_STRING, ID_MAIN_FILE_RECORDAVI, m_recordingAvi ? _T("Stop Recording AVI") : _T("Record AVI...")); 
	}

	//Fix the virtual machine sub menu
	{
		HMENU hMenu = GetSubMenu(GetMenu(m_hWnd), VMMENUPOS);

		bool hasElf = (os.GetELF() != NULL);

		EnableMenuItem(hMenu, ID_MAIN_VM_RESUME, (!hasElf ? MF_GRAYED : 0) | MF_BYCOMMAND);
		EnableMenuItem(hMenu, ID_MAIN_VM_RESET, (!hasElf ? MF_GRAYED : 0) | MF_BYCOMMAND);
		CheckMenuItem(hMenu, ID_MAIN_VM_PAUSEFOCUS, (m_nPauseFocusLost ? MF_CHECKED : MF_UNCHECKED) | MF_BYCOMMAND);
		EnableMenuItem(hMenu, ID_MAIN_VM_SAVESTATE, (!hasElf ? MF_GRAYED : 0) | MF_BYCOMMAND);
		EnableMenuItem(hMenu, ID_MAIN_VM_LOADSTATE, (!hasElf ? MF_GRAYED : 0) | MF_BYCOMMAND);

		//Get state slot sub-menu
		MENUITEMINFO MenuItem;
		memset(&MenuItem, 0, sizeof(MENUITEMINFO));
		MenuItem.cbSize = sizeof(MENUITEMINFO);
		MenuItem.fMask	= MIIM_SUBMENU;

		GetMenuItemInfo(hMenu, ID_MAIN_VM_STATESLOT, FALSE, &MenuItem);
		hMenu = MenuItem.hSubMenu;

		//Change state slot number checkbox
		for(unsigned int i = 0; i < MAX_STATESLOTS; i++)
		{
			memset(&MenuItem, 0, sizeof(MENUITEMINFO));
			MenuItem.cbSize = sizeof(MENUITEMINFO);
			MenuItem.fMask	= MIIM_STATE;
			MenuItem.fState	= (m_nStateSlot == i) ? MFS_CHECKED : MFS_UNCHECKED;

			SetMenuItemInfo(hMenu, ID_MAIN_VM_STATESLOT_0 + i, FALSE, &MenuItem);
		}
	}

	TCHAR sTitle[256];
	const char* sExec = os.GetExecutableName();
	if(strlen(sExec))
	{
		_sntprintf(sTitle, countof(sTitle), _T("%s - [ %s ]"), APP_NAME, string_cast<std::tstring>(sExec).c_str());
	}
	else
	{
		_sntprintf(sTitle, countof(sTitle), _T("%s"), APP_NAME);
	}

	SetText(sTitle);
}

void CMainWindow::PrintVersion(TCHAR* sVersion, size_t nCount)
{
	_sntprintf(sVersion, nCount, APP_NAME _T(" v%s - %s"), APP_VERSIONSTR, string_cast<std::tstring>(__DATE__).c_str());
}

void CMainWindow::OnOutputWndSizeChange()
{
	RefreshLayout();
}

void CMainWindow::OnNewFrame(uint32 drawCallCount)
{
	if(m_recordingAvi)
	{
		WaitForSingleObject(m_recordAviMutex, INFINITE);

		m_virtualMachine.m_pGS->ReadFramebuffer(m_recordBufferWidth, m_recordBufferHeight, m_recordBuffer);
		m_aviStream.Write(m_recordBuffer);

		ReleaseMutex(m_recordAviMutex);
	}

	m_nFrames++;
	m_drawCallCount += drawCallCount;
}

void CMainWindow::OnExecutableChange()
{
	UpdateUI();
}

CMainWindow::CScopedVmPauser::CScopedVmPauser(CPS2VM& virtualMachine)
: m_virtualMachine(virtualMachine)
, m_paused(false)
{
	if(m_virtualMachine.GetStatus() == CVirtualMachine::RUNNING)
	{
		m_paused = true;
		m_virtualMachine.Pause();
	}
}

CMainWindow::CScopedVmPauser::~CScopedVmPauser()
{
	if(m_paused)
	{
		m_virtualMachine.Resume();
	}
}

void CMainWindow::CBootCdRomOpenCommand::Execute(CMainWindow* mainWindow)
{
	mainWindow->BootCDROM();
}

CMainWindow::CLoadElfOpenCommand::CLoadElfOpenCommand(const char* fileName) :
m_fileName(fileName)
{

}

void CMainWindow::CLoadElfOpenCommand::Execute(CMainWindow* mainWindow)
{
	mainWindow->LoadELF(m_fileName.c_str());
}
