/*
This is a part of the LiteStep Shell Source code.

Copyright (C) 1997-2002 The LiteStep Development Team

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/ 
/****************************************************************************
****************************************************************************/
#include <process.h>
#include "module.h"
#include "../utility/macros.h"
#include "../lsapi/ThreadedBangCommand.h"
#include "../utility/core.hpp"


Module::Module(LPCTSTR ptzLoc, DWORD dwFlags)
{
	m_hInstance = NULL;
	m_hThread = NULL;
	m_pInitEx = NULL;
	m_hInitEvent = NULL;
	m_hQuitEvent = NULL;
	m_pQuit = NULL;
	m_dwFlags = dwFlags;

	if (IsValidStringPtr(ptzLoc))
	{
		m_tzLocation = ptzLoc;
	}
}


bool Module::_LoadDll()
{
    bool bReturn = false;

	if (!m_hInstance)
	{
		m_hInstance = LoadLibrary(m_tzLocation.c_str());
		
		if (m_hInstance)
		{
			m_pInitEx = (ModuleInitExFunc)GetProcAddress(m_hInstance, "initModuleEx");
			if (!m_pInitEx) // Might be a BC module, check for underscore
			{
				m_pInitEx = (ModuleInitExFunc)GetProcAddress(m_hInstance, "_initModuleEx");
			}

			m_pQuit = (ModuleQuitFunc)GetProcAddress(m_hInstance, "quitModule");
			if (!m_pQuit)   // Might be a BC module, check for underscore
			{
                m_pQuit = (ModuleQuitFunc)GetProcAddress(m_hInstance, "_quitModule");
			}

			if (!m_pInitEx)
			{
				RESOURCE_STR(NULL, IDS_INITMODULEEXNOTFOUND_ERROR,
					"Error: Could not find initModuleEx().\n\nPlease confirm that the dll is a Litestep module,\nand check with the author for updates.");
			}
			else if (!m_pQuit)
			{
				RESOURCE_STR(NULL, IDS_QUITMODULENOTFOUND_ERROR,
					"Error: Could not find quitModule().\n\nPlease conirm that the dll is a Litestep module.");
			}
			else
			{
				bReturn = true;
			}
		}
		else
		{
			RESOURCE_STR(NULL, IDS_MODULENOTFOUND_ERROR,
				"Error: Could not locate module.\nPlease check your configuration.");
		}
	}

	if (!bReturn)
	{
		MessageBox(NULL, resourceTextBuffer, m_tzLocation.c_str(),
			MB_OK | MB_ICONEXCLAMATION | MB_TOPMOST | MB_SETFOREGROUND);

		if (m_hInstance)
		{
			FreeLibrary(m_hInstance);
			m_hInstance = NULL;
		}
	}

	return bReturn;
}


Module::~Module()
{
    if (m_dwFlags & LS_MODULE_THREADED)
    {
        CloseHandle(m_hThread);
        CloseHandle(m_hInitEvent);
        CloseHandle(m_hQuitEvent);
    }
    
	if (m_hInstance)
	{
		FreeLibrary(m_hInstance);
		m_hInstance = NULL;
	}
}

HANDLE Module::Init(HWND hMainWindow, LPCTSTR ptzAppPath)
{
    ASSERT(m_hInstance == NULL);
    
    // delaying the LoadLibrary call until this point is necessary to make
    // grdtransparent work (it hooks LoadLibrary)
    if (_LoadDll())
	{
		ASSERT(m_pInitEx != NULL);
        ASSERT(m_pQuit != NULL);

        m_hMainWindow = hMainWindow;
        m_tzAppPath = ptzAppPath;
        
        if (m_dwFlags & LS_MODULE_THREADED)
        {
            SECURITY_ATTRIBUTES sa;
            
            sa.nLength = sizeof(SECURITY_ATTRIBUTES);
            sa.lpSecurityDescriptor = NULL;
            sa.bInheritHandle = FALSE;
            
            m_hInitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
            // using _beginthreadex instead of CreateThread because modules
            // might use CRT functions
            m_hThread = (HANDLE)_beginthreadex(&sa, 0, Module::ThreadProc,
                this, 0, (UINT*)&m_dwThreadID);
        }
        else
        {
            CallInit();
        }
	}

	return m_hInitEvent;
}

int Module::CallInit()
{
	int nReturn = 0;

	try
	{
		nReturn = m_pInitEx(m_hMainWindow, m_hInstance, m_tzAppPath.c_str());
	}
    catch (...)
    {
        RESOURCE_MSGBOX(NULL, IDS_MODULEINITEXCEPTION_ERROR,
            "Error: Exception during module initialization.\n\nPlease contact the module writer.",
            m_tzLocation.c_str());
    }

    return nReturn;
}


void Module::CallQuit()
{
    if (m_pQuit)
    {
        try
        {
            m_pQuit(m_hInstance);
        }
        catch (...)
        {
            RESOURCE_MSGBOX(NULL, IDS_MODULEQUIT_ERROR,
                "Exception while quitting module.", m_tzLocation.c_str())
        }
    }
}


HANDLE Module::Quit()
{
	if (m_hInstance)
	{
		if (m_hThread)
		{
			m_hQuitEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
			PostThreadMessage(m_dwThreadID, WM_DESTROY, 0, (LPARAM)this);
		}
		else
		{
            CallQuit();
		}
	}

	return m_hQuitEvent;
}


UINT __stdcall Module::ThreadProc(void* dllModPtr)
{
	Module* dllMod = (Module*)dllModPtr;

	dllMod->CallInit();

    SetEvent(dllMod->GetInitEvent());
    
    if (dllMod->HasMessagePump())
    {
        MSG msg;
        
        while (GetMessage(&msg, 0, 0, 0))
        {
            try
            {
                if (msg.hwnd == NULL)
                {
                    // Thread message
                    dllMod->HandleThreadMessage(msg);
                }
                else
                {
                    // Window message
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
            catch (...)
            {
                // Quietly ignore exceptions?
                // #pragma COMPILE_WARN(Note: Need stronger exception-handling code here...restart the module or something)
			}
		}
	}

	SetEvent(dllMod->GetQuitEvent());

	return 0;
}

void Module::HandleThreadMessage(MSG &msg)
{
	switch (msg.message)
	{
		case LM_THREAD_BANGCOMMAND:
		{
			ThreadedBangCommand * pInfo = (ThreadedBangCommand*)msg.wParam;

			if (pInfo != NULL)
			{
				pInfo->Execute();
				pInfo->Release(); //check BangCommand.cpp for the reason
			}
		}
		break;

		case WM_DESTROY:
		{
			Module *dll_mod = (Module*)msg.lParam;
			
            if (dll_mod)
			{
				dll_mod->CallQuit();
                PostQuitMessage(0);
            }
		}
		break;

        default:
        break;
	}
}
