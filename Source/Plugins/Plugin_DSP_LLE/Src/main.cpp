// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/


#include "Common.h" // Common
#include "CommonTypes.h"
#include "LogManager.h"
#include "Thread.h"
#include "ChunkFile.h"

#include "Globals.h" // Local
#include "DSPInterpreter.h"
#include "DSPHWInterface.h"
#include "disassemble.h"
#include "DSPSymbols.h"
#include "Config.h"

#include "AudioCommon.h"
#include "Mixer.h"

#include "DSPTables.h"
#include "DSPCore.h"

#if defined(HAVE_WX) && HAVE_WX
#include "DSPConfigDlgLLE.h"
DSPConfigDialogLLE* m_ConfigFrame = NULL;
#include "Debugger/DSPDebugWindow.h"
DSPDebuggerLLE* m_DebuggerFrame = NULL;
#endif

PLUGIN_GLOBALS* globals = NULL;
DSPInitialize g_dspInitialize;
Common::Thread *g_hDSPThread = NULL;
SoundStream *soundStream = NULL;
bool g_InitMixer = false;

bool bIsRunning = false;

// Standard crap to make wxWidgets happy
#ifdef _WIN32
HINSTANCE g_hInstance;

#if defined(HAVE_WX) && HAVE_WX
class wxDLLApp : public wxApp
{
	bool OnInit()
	{
		return true;
	}
};
IMPLEMENT_APP_NO_MAIN(wxDLLApp) 
WXDLLIMPEXP_BASE void wxSetInstance(HINSTANCE hInst);
#endif

BOOL APIENTRY DllMain(HINSTANCE hinstDLL,	// DLL module handle
					  DWORD dwReason,		// reason called
					  LPVOID lpvReserved)	// reserved
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		{
#if defined(HAVE_WX) && HAVE_WX
			wxSetInstance((HINSTANCE)hinstDLL);
			wxInitialize();
#endif
		}
		break; 

	case DLL_PROCESS_DETACH:
#if defined(HAVE_WX) && HAVE_WX
		wxUninitialize();
#endif
		break;
	}

	g_hInstance = hinstDLL;
	return TRUE;
}
#endif

#if defined(HAVE_WX) && HAVE_WX
wxWindow* GetParentedWxWindow(HWND Parent)
{
#ifdef _WIN32
	wxSetInstance((HINSTANCE)g_hInstance);
#endif
	wxWindow *win = new wxWindow();
#ifdef _WIN32
	win->SetHWND((WXHWND)Parent);
	win->AdoptAttributesFromHWND();
#endif
	return win;
}
#endif


void GetDllInfo(PLUGIN_INFO* _PluginInfo)
{
	_PluginInfo->Version = 0x0100;
	_PluginInfo->Type = PLUGIN_TYPE_DSP;

#ifdef DEBUGFAST
	sprintf(_PluginInfo->Name, "Dolphin DSP-LLE Plugin (DebugFast)");
#else
#ifndef _DEBUG
	sprintf(_PluginInfo->Name, "Dolphin DSP-LLE Plugin");
#else
	sprintf(_PluginInfo->Name, "Dolphin DSP-LLE Plugin (Debug)");
#endif
#endif
}

void SetDllGlobals(PLUGIN_GLOBALS* _pPluginGlobals)
{
	globals = _pPluginGlobals;
	LogManager::SetInstance((LogManager *)globals->logManager);
}

void DllConfig(HWND _hParent)
{
#if defined(HAVE_WX) && HAVE_WX
	wxWindow *frame = GetParentedWxWindow(_hParent);
	m_ConfigFrame = new DSPConfigDialogLLE(frame);

	// add backends
	std::vector<std::string> backends = AudioCommon::GetSoundBackends();

	for (std::vector<std::string>::const_iterator iter = backends.begin(); 
		 iter != backends.end(); ++iter)
	{
		m_ConfigFrame->AddBackend((*iter).c_str());
	}

	// Only allow one open at a time
#ifdef _WIN32
	frame->Disable();
	m_ConfigFrame->ShowModal();
	frame->Enable();
#else
	m_ConfigFrame->ShowModal();
#endif

#ifdef _WIN32
	frame->SetFocus();
	frame->SetHWND(NULL);
#endif

	m_ConfigFrame->Destroy();
	m_ConfigFrame = NULL;
	frame->Destroy();
#endif
}

void DoState(unsigned char **ptr, int mode)
{
	PointerWrap p(ptr, mode);
	p.Do(g_InitMixer);

// Enable this when the HLE is fixed to save/load the same amount of data,
// no matter how bogus, so that one can switch LLE->HLE. The other way is unlikely to work very well.
#if 0
	p.Do(g_dsp.r);
	p.Do(g_dsp.pc);
	p.Do(g_dsp.err_pc);
	p.Do(g_dsp.cr);
	p.Do(g_dsp.reg_stack_ptr);
	p.Do(g_dsp.exceptions);
	p.Do(g_dsp.exceptions_in_progress);
	for (int i = 0; i < 4; i++) {
		p.Do(g_dsp.reg_stack[i]);
	}
	p.Do(g_dsp.iram_crc);
	p.Do(g_dsp.step_counter);
	p.Do(g_dsp.ifx_regs);
	p.Do(g_dsp.mbox[0]);
	p.Do(g_dsp.mbox[1]);
	p.DoArray(g_dsp.iram, DSP_IRAM_BYTE_SIZE);
	p.DoArray(g_dsp.dram, DSP_DRAM_BYTE_SIZE);
#endif
}

void EmuStateChange(PLUGIN_EMUSTATE newState)
{
	DSP_ClearAudioBuffer((newState == PLUGIN_EMUSTATE_PLAY) ? false : true);
}

void DllDebugger(HWND _hParent, bool Show)
{
#if defined(HAVE_WX) && HAVE_WX
	if (!m_DebuggerFrame)
		m_DebuggerFrame = new DSPDebuggerLLE(GetParentedWxWindow(_hParent));

	if (Show)
		m_DebuggerFrame->Show();
	else
		m_DebuggerFrame->Hide();
#endif
}


// Regular thread
THREAD_RETURN dsp_thread(void* lpParameter)
{
	while (bIsRunning)
	{
		DSPInterpreter::Run();
	}
	return 0;
}

void DSP_DebugBreak()
{
#if defined(HAVE_WX) && HAVE_WX
	// if (m_DebuggerFrame)
	//  	m_DebuggerFrame->DebugBreak();
#endif
}

void Initialize(void *init)
{
	g_InitMixer = false;
    bool bCanWork = true;
    g_dspInitialize = *(DSPInitialize*)init;

	g_Config.Load();

	std::string irom_filename = File::GetSysDirectory() + GC_SYS_DIR + DIR_SEP + DSP_IROM;
	std::string coef_filename = File::GetSysDirectory() + GC_SYS_DIR + DIR_SEP + DSP_COEF;
	bCanWork = DSPCore_Init(irom_filename.c_str(), coef_filename.c_str());

	g_dsp.cpu_ram = g_dspInitialize.pGetMemoryPointer(0);
	DSPCore_Reset();

	if (!bCanWork)
	{
		PanicAlert("DSPLLE: Failed to initialize plugin, exiting");
		DSPCore_Shutdown();
		return;
	}

	bIsRunning = true;

	InitInstructionTable();

	if (g_dspInitialize.bOnThread)
	{
		g_hDSPThread = new Common::Thread(dsp_thread, NULL);
	}

#if defined(HAVE_WX) && HAVE_WX
	if (m_DebuggerFrame)
		m_DebuggerFrame->Refresh();
#endif
}

void DSP_StopSoundStream()
{
	DSPInterpreter::Stop();
	bIsRunning = false;
	if (g_dspInitialize.bOnThread)
	{
		delete g_hDSPThread;
		g_hDSPThread = NULL;
	}
}

void Shutdown()
{
	AudioCommon::ShutdownSoundStream();
	DSPCore_Shutdown();
}

u16 DSP_WriteControlRegister(u16 _uFlag)
{
	UDSPControl Temp(_uFlag);
	if (!g_InitMixer)
	{
		if (!Temp.DSPHalt && Temp.DSPInit)
		{
			unsigned int AISampleRate, DACSampleRate;
			g_dspInitialize.pGetSampleRate(AISampleRate, DACSampleRate);
			soundStream = AudioCommon::InitSoundStream(new CMixer(AISampleRate, DACSampleRate)); 
			if(!soundStream) PanicAlert("Error starting up sound stream");
			// Mixer is initialized
			g_InitMixer = true;
		}
	}
	DSPInterpreter::WriteCR(_uFlag);
	return DSPInterpreter::ReadCR();
}

u16 DSP_ReadControlRegister()
{
	return DSPInterpreter::ReadCR();
}

u16 DSP_ReadMailboxHigh(bool _CPUMailbox)
{
	if (_CPUMailbox)
		return gdsp_mbox_read_h(GDSP_MBOX_CPU);
	else
		return gdsp_mbox_read_h(GDSP_MBOX_DSP);
}

u16 DSP_ReadMailboxLow(bool _CPUMailbox)
{
	if (_CPUMailbox)
		return gdsp_mbox_read_l(GDSP_MBOX_CPU);
	else
		return gdsp_mbox_read_l(GDSP_MBOX_DSP);
}

void DSP_WriteMailboxHigh(bool _CPUMailbox, u16 _uHighMail)
{
	if (_CPUMailbox)
	{
		if (gdsp_mbox_peek(GDSP_MBOX_CPU) & 0x80000000)
		{
			ERROR_LOG(DSPLLE, "Mailbox isnt empty ... strange");
		}

#if PROFILE
		if ((_uHighMail) == 0xBABE)
		{
			ProfilerStart();
		}
#endif

		gdsp_mbox_write_h(GDSP_MBOX_CPU, _uHighMail);
	}
	else
	{
		ERROR_LOG(DSPLLE, "CPU cant write to DSP mailbox");
	}
}

void DSP_WriteMailboxLow(bool _CPUMailbox, u16 _uLowMail)
{
	if (_CPUMailbox)
	{
		gdsp_mbox_write_l(GDSP_MBOX_CPU, _uLowMail);
	}
	else
	{
		ERROR_LOG(DSPLLE, "CPU cant write to DSP mailbox");
	}
}

void DSP_Update(int cycles)
{
// Sound stream update job has been handled by AudioDMA routine, which is more efficient
/*
	// This gets called VERY OFTEN. The soundstream update might be expensive so only do it 200 times per second or something.
	int cycles_between_ss_update;

	if (g_dspInitialize.bWii)
		cycles_between_ss_update = 121500000 / 200;
	else
		cycles_between_ss_update = 81000000 / 200;
	
	static int cycle_count = 0;
	cycle_count += cycles;
	if (cycle_count > cycles_between_ss_update)
	{
		while (cycle_count > cycles_between_ss_update)
			cycle_count -= cycles_between_ss_update;
		soundStream->Update();
	}
*/
	// If we're not on a thread, run cycles here.
	if (!g_dspInitialize.bOnThread)
	{
		DSPCore_RunCycles(cycles);
	}
}

void DSP_SendAIBuffer(unsigned int address, unsigned int num_samples)
{
	if (!soundStream)
		return;

	CMixer *pMixer = soundStream->GetMixer();

	if (pMixer != 0 && address != 0)
	{
		short *samples = (short *)Memory_Get_Pointer(address);
		pMixer->PushSamples(samples, num_samples);
	}

	soundStream->Update();
}

void DSP_ClearAudioBuffer(bool mute)
{
	if (soundStream)
		soundStream->Clear(mute);
}

