; NppSync.nsi — double-click installer for the Notepad++ Sync plugin.
;
; Packaging only. This script copies NppSync.dll (and deps\ when the build
; has that folder) into <Notepad++>\plugins\NppSync\. It does not write
; settings, a server address, Google sign-in data, pairing data, or keys.
; A fresh install keeps the Backend URL compiled into the plugin.
;
; Build (from the repo, after package.ps1 resolves the DLL):
;   makensis -DVERSION=1.2.0 -DVERSION_VI=1.2.0.0 ^
;     -DPLUGIN_DLL=C:\path\NppSync.dll ^
;     -DLICENSE_FILE=C:\path\LICENSE ^
;     -DOUTFILE=C:\path\NotepadPlusPlusSync-v1.2.0-win64-setup.exe ^
;     installer\NppSync.nsi
;
; Optional, only when deps\ contains files:
;   -DINCLUDE_DEPS -DPLUGIN_DEPS=C:\path\deps

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"
!include "FileFunc.nsh"
!include "WinMessages.nsh"

!insertmacro GetParameters
!insertmacro GetOptions
!insertmacro GetFileName
!insertmacro GetParent

!ifndef VERSION
  !define VERSION "1.2.0"
!endif
!ifndef VERSION_VI
  !define VERSION_VI "${VERSION}.0"
!endif
!ifndef LICENSE_FILE
  !define LICENSE_FILE "..\LICENSE"
!endif
!ifndef OUTFILE
  !define OUTFILE "NotepadPlusPlusSync-v${VERSION}-win64-setup.exe"
!endif
!ifndef PLUGIN_DLL
  !error "PLUGIN_DLL is required. Pass /DPLUGIN_DLL=full\path\to\NppSync.dll"
!endif
!ifdef INCLUDE_DEPS
  !ifndef PLUGIN_DEPS
    !error "PLUGIN_DEPS is required when INCLUDE_DEPS is set"
  !endif
!endif

Name "Notepad++ Sync"
Caption "Notepad++ Sync ${VERSION} Setup"
OutFile "${OUTFILE}"
Unicode true
; "highest" shows one Windows permission prompt for an administrator.
; A standard user is not prompted yet; Program Files relaunches with runas.
RequestExecutionLevel highest
ManifestDPIAware true
SetCompressor /SOLID lzma
BrandingText "Notepad++ Sync"
InstallDir "$PROGRAMFILES64\Notepad++"

!define MUI_ABORTWARNING
!define MUI_ABORTWARNING_TEXT "Stop installing Notepad++ Sync?"

!define MUI_WELCOMEPAGE_TITLE "Install Notepad++ Sync"
!define MUI_WELCOMEPAGE_TEXT "This adds the Notepad++ Sync plugin. You do not copy any files yourself.$\r$\n$\r$\nIf Windows asks for permission, choose Yes. That only allows the plugin to be copied into the Notepad++ folder.$\r$\n$\r$\nIf Notepad++ is open, you will be asked before it is closed. Notepad++ will let you save unsaved notes. This setup will not force it to quit.$\r$\n$\r$\nYou are not asked for a server address."

!define MUI_DIRECTORYPAGE_TEXT_TOP "Choose the folder that contains 64-bit notepad++.exe. Do not choose the plugins folder. The installer creates plugins\NppSync inside the folder you pick."
!define MUI_DIRECTORYPAGE_TEXT_DESTINATION "Notepad++ folder"

!define MUI_FINISHPAGE_TITLE "Notepad++ Sync is installed"
!define MUI_FINISHPAGE_TEXT "Open Notepad++ and use Plugins, then Notepad++ Sync.$\r$\n$\r$\nYou were not asked for a server address. A new install already knows where to connect. Sign-in and encryption settings already on this PC were left as they are."
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Open Notepad++"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchNpp
!define MUI_FINISHPAGE_RUN_CHECKED

!define MUI_UNCONFIRMPAGE_TEXT_TOP "This removes the Notepad++ Sync plugin.$\r$\n$\r$\nYour notes, sign-in, and encryption key in %APPDATA%\Notepad++Sync are left in place."
!define MUI_UNCONFIRMPAGE_TEXT_LOCATION "Plugin folder"

!insertmacro MUI_PAGE_WELCOME
; Abort in the creator, before the dialog exists, skips this page.
Page custom ConfirmCreate ConfirmLeave
!define MUI_PAGE_CUSTOMFUNCTION_PRE DirPre
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE DirLeave
!define MUI_DIRECTORYPAGE_VERIFYONLEAVE
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var NppFound
Var Found32
Var Candidate32
Var DirWritable
Var Elevated
Var NppRunning
Var WriteTestDir

; ---------------------------------------------------------------------------
; Close Notepad++ only after an obvious Yes. WM_CLOSE lets Notepad++ ask
; the user to save. This installer never terminates the process.
; ---------------------------------------------------------------------------
!macro IsNppRunning UN
Function ${UN}IsNppRunning
  StrCpy $NppRunning "0"
  nsExec::ExecToStack 'tasklist /FI "IMAGENAME eq notepad++.exe" /NH'
  Pop $R0
  Pop $R1
  StrCpy $R2 "$R1"
${UN}scanTask:
  StrCpy $R3 "$R2" 13
  ${If} $R3 == "notepad++.exe"
    StrCpy $NppRunning "1"
    Return
  ${EndIf}
  ${If} "$R2" == ""
    Goto ${UN}scanDone
  ${EndIf}
  StrCpy $R2 "$R2" "" 1
  Goto ${UN}scanTask
${UN}scanDone:
  System::Call 'user32::FindWindowExW(p 0, p 0, w "Notepad++", p 0) p .R8'
  ${If} $R8 != 0
    StrCpy $NppRunning "1"
  ${EndIf}
FunctionEnd
!macroend

!macro PostCloseNpp UN
Function ${UN}PostCloseNpp
  StrCpy $R9 0
${UN}nextWnd:
  System::Call 'user32::FindWindowExW(p 0, p r9, w "Notepad++", p 0) p .R8'
  ${If} $R8 == 0
    Return
  ${EndIf}
  ; WM_CLOSE = 0x0010. Notepad++ prompts to save. Do not use TerminateProcess.
  System::Call 'user32::PostMessageW(p r8, i 16, p 0, p 0)'
  StrCpy $R9 $R8
  Goto ${UN}nextWnd
FunctionEnd
!macroend

!macro EnsureNppClosed UN
Function ${UN}EnsureNppClosed
${UN}ask:
  Call ${UN}IsNppRunning
  ${If} $NppRunning != "1"
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONEXCLAMATION \
    "Notepad++ is open.$\r$\n$\r$\nThe plugin files cannot be changed while Notepad++ is open.$\r$\n$\r$\nYes — ask Notepad++ to close. It will let you save unsaved notes. This setup will not force it to quit.$\r$\n$\r$\nNo — stop. Close Notepad++ yourself, then run this setup again." \
    /SD IDNO IDYES ${UN}doClose
  SetErrorLevel 2
  Quit
${UN}doClose:
  Call ${UN}PostCloseNpp
  StrCpy $R7 0
${UN}waitLoop:
  IntCmp $R7 30 ${UN}stillOpen
  Sleep 1000
  Call ${UN}IsNppRunning
  ${If} $NppRunning != "1"
    Return
  ${EndIf}
  IntOp $R7 $R7 + 1
  Goto ${UN}waitLoop
${UN}stillOpen:
  MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION \
    "Notepad++ is still open.$\r$\n$\r$\nSave your notes and close Notepad++, then choose Retry.$\r$\n$\r$\nCancel stops this setup." \
    /SD IDCANCEL IDRETRY ${UN}ask
  SetErrorLevel 2
  Quit
FunctionEnd
!macroend

!macro TestWritable UN
Function ${UN}TestWritable
  StrCpy $DirWritable "0"
  ClearErrors
  CreateDirectory "$WriteTestDir"
  FileOpen $R9 "$WriteTestDir\.__npsync_write_test" w
  ${IfNot} ${Errors}
    FileClose $R9
    Delete "$WriteTestDir\.__npsync_write_test"
    StrCpy $DirWritable "1"
    ; Drop a folder this test just created. RMDir does nothing when the
    ; folder already holds the plugin.
    ClearErrors
    RMDir "$WriteTestDir"
    ClearErrors
  ${EndIf}
FunctionEnd
!macroend

!macro IsElevated UN
Function ${UN}IsElevated
  ; TokenElevation (20), not group membership. An administrator who has not
  ; approved the Windows prompt is not elevated.
  StrCpy $Elevated "0"
  System::Call 'kernel32::GetCurrentProcess()p.R1'
  System::Call 'advapi32::OpenProcessToken(p R1, i 8, *p .R2)i.R3'
  ${If} $R3 == 0
    Return
  ${EndIf}
  System::Call 'advapi32::GetTokenInformation(p R2, i 20, *i .R4, i 4, *i .R5)i.R3'
  ${If} $R3 != 0
  ${AndIf} $R4 != 0
    StrCpy $Elevated "1"
  ${EndIf}
  System::Call 'kernel32::CloseHandle(p R2)'
FunctionEnd
!macroend

!insertmacro IsNppRunning ""
!insertmacro IsNppRunning "un."
!insertmacro PostCloseNpp ""
!insertmacro PostCloseNpp "un."
!insertmacro EnsureNppClosed ""
!insertmacro EnsureNppClosed "un."
!insertmacro TestWritable ""
!insertmacro TestWritable "un."
!insertmacro IsElevated ""
!insertmacro IsElevated "un."

Function NormalizeCandidate
  ; $R8 in/out. Turns an exe path or a directory into a directory.
  ${If} "$R8" == ""
    Return
  ${EndIf}
  StrCpy $R7 "$R8" 1
  ${If} $R7 == '"'
    StrCpy $R8 "$R8" "" 1
  ${EndIf}
  StrCpy $R7 "$R8" 1 -1
  ${If} $R7 == '"'
    StrCpy $R8 "$R8" -1
  ${EndIf}
normSlash:
  ${If} "$R8" == ""
    Return
  ${EndIf}
  StrCpy $R7 "$R8" 1 -1
  ${If} $R7 == "\"
  ${OrIf} $R7 == "/"
    StrCpy $R8 "$R8" -1
    Goto normSlash
  ${EndIf}
  ${GetFileName} "$R8" $R7
  ${If} $R7 == "notepad++.exe"
    ${GetParent} "$R8" $R8
  ${EndIf}
FunctionEnd

Function ValidateInstalledDir
  ; $INSTDIR in. Sets $NppFound / $Candidate32.
  StrCpy $NppFound "0"
  StrCpy $Candidate32 "0"
  ${If} "$INSTDIR" == ""
    Return
  ${EndIf}
  IfFileExists "$INSTDIR\notepad++.exe" 0 validateDone
  ; SCS_64BIT_BINARY = 6. Rejects 32-bit Notepad++.
  ; Pass the path in a register so a backslash in "\notepad++.exe" is not
  ; treated as an escape by System::Call.
  StrCpy $R6 "$INSTDIR\notepad++.exe"
  System::Call 'kernel32::GetBinaryTypeW(w R6, *i .R5) i .R4'
  ${If} $R4 != 0
  ${AndIf} $R5 = 6
    StrCpy $NppFound "1"
  ${Else}
    StrCpy $Candidate32 "1"
  ${EndIf}
validateDone:
FunctionEnd

Function TryCandidate
  ${If} $NppFound == "1"
    Return
  ${EndIf}
  ${If} "$R8" == ""
    Return
  ${EndIf}
  Call NormalizeCandidate
  StrCpy $INSTDIR "$R8"
  Call ValidateInstalledDir
  ${If} $NppFound != "1"
    ${If} $Candidate32 == "1"
      StrCpy $Found32 "1"
    ${EndIf}
    StrCpy $INSTDIR ""
  ${EndIf}
FunctionEnd

; Root key must be a literal. SetRegView 64 is already on, so HKLM is the
; 64-bit view (not WOW6432Node).
!macro ReadInstallKeys HIVE
  ClearErrors
  ReadRegStr $R8 ${HIVE} "Software\Notepad++" ""
  ClearErrors
  Call TryCandidate
  ClearErrors
  ReadRegStr $R8 ${HIVE} "Software\Microsoft\Windows\CurrentVersion\Uninstall\Notepad++" "InstallLocation"
  ClearErrors
  Call TryCandidate
  ClearErrors
  ReadRegStr $R8 ${HIVE} "Software\Microsoft\Windows\CurrentVersion\App Paths\notepad++.exe" ""
  ClearErrors
  Call TryCandidate
!macroend

Function FindNotepad
  StrCpy $NppFound "0"
  StrCpy $Found32 "0"
  StrCpy $INSTDIR ""
  SetRegView 64

  !insertmacro ReadInstallKeys HKLM
  StrCpy $R8 "$PROGRAMFILES64\Notepad++"
  Call TryCandidate

  !insertmacro ReadInstallKeys HKCU
  StrCpy $R8 "$LOCALAPPDATA\Programs\Notepad++"
  Call TryCandidate
  StrCpy $R8 "$LOCALAPPDATA\Notepad++"
  Call TryCandidate

  ; 32-bit install is not a destination. Remember it so the message is specific.
  ${If} $NppFound != "1"
    StrCpy $R8 "$PROGRAMFILES32\Notepad++"
    Call TryCandidate
  ${EndIf}

  ${If} $NppFound != "1"
    StrCpy $INSTDIR "$PROGRAMFILES64\Notepad++"
  ${EndIf}
FunctionEnd

Function RelaunchElevated
  Call IsElevated
  ${If} $Elevated == "1"
    Return
  ${EndIf}
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/elevated" $R1
  ${IfNot} ${Errors}
    MessageBox MB_ICONSTOP "Windows did not allow installing into$\r$\n$INSTDIR$\r$\n$\r$\nChoose Yes when Windows asks for permission."
    SetErrorLevel 2
    Quit
  ${EndIf}
  ; A trailing backslash would escape the closing quote on the command line.
  StrCpy $R7 "$INSTDIR" 1 -1
  ${If} $R7 == "\"
    StrCpy $INSTDIR "$INSTDIR" -1
  ${EndIf}
  StrCpy $R9 '/elevated /NppDir="$INSTDIR"'
  ClearErrors
  ExecShell "runas" "$EXEPATH" "$R9"
  SetErrorLevel 0
  Quit
FunctionEnd

Function un.RelaunchElevated
  Call un.IsElevated
  ${If} $Elevated == "1"
    Return
  ${EndIf}
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/elevated" $R1
  ${IfNot} ${Errors}
    MessageBox MB_ICONSTOP "Windows did not allow removing the plugin from$\r$\n$INSTDIR$\r$\n$\r$\nChoose Yes when Windows asks for permission."
    SetErrorLevel 2
    Quit
  ${EndIf}
  ClearErrors
  ExecShell "runas" "$EXEPATH" "/elevated"
  SetErrorLevel 0
  Quit
FunctionEnd

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "Notepad++ Sync is a 64-bit plugin. It needs 64-bit Windows and 64-bit Notepad++."
    SetErrorLevel 2
    Quit
  ${EndIf}

  Call FindNotepad
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/NppDir=" $R1
  ${IfNot} ${Errors}
    ${If} "$R1" != ""
      StrCpy $INSTDIR "$R1"
      Call ValidateInstalledDir
    ${EndIf}
  ${EndIf}

  ${If} $NppFound == "1"
    StrCpy $WriteTestDir "$INSTDIR\plugins\NppSync"
    Call TestWritable
    ${If} $DirWritable != "1"
      Call RelaunchElevated
      ${If} $DirWritable != "1"
        MessageBox MB_ICONSTOP "Windows did not allow installing into$\r$\n$INSTDIR\plugins\NppSync"
        SetErrorLevel 2
        Quit
      ${EndIf}
    ${EndIf}
  ${Else}
    IfSilent 0 notSilentMissing
    SetErrorLevel 2
    Quit
notSilentMissing:
    ${If} $Found32 == "1"
      MessageBox MB_OK|MB_ICONEXCLAMATION "A 32-bit Notepad++ was found. This plugin needs 64-bit Notepad++.$\r$\n$\r$\nIf 64-bit Notepad++ is installed in another folder, choose that folder on the next page. Otherwise install 64-bit Notepad++ and run this setup again."
    ${EndIf}
  ${EndIf}
FunctionEnd

Function un.onInit
  SetRegView 64
  StrCpy $WriteTestDir "$INSTDIR"
  Call un.TestWritable
  ${If} $DirWritable != "1"
    Call un.RelaunchElevated
  ${EndIf}
FunctionEnd

Function ConfirmCreate
  ${If} $NppFound != "1"
    Abort
  ${EndIf}
  !insertmacro MUI_HEADER_TEXT "Install location" "Notepad++ Sync will be copied into this Notepad++."
  nsDialogs::Create 1018
  Pop $R0
  ${If} $R0 == error
    Abort
  ${EndIf}
  ${NSD_CreateLabel} 0 0 100% 24u "Notepad++ was found in this folder:"
  Pop $R1
  ${NSD_CreateLabel} 0 28u 100% 32u "$INSTDIR"
  Pop $R1
  ${NSD_CreateLabel} 0 68u 100% 48u "Install copies the plugin into plugins\NppSync. You are not asked for a server address. Notes, sign-in, and encryption settings already on this PC stay where they are."
  Pop $R1
  GetDlgItem $R1 $HWNDPARENT 1
  SendMessage $R1 ${WM_SETTEXT} 0 "STR:&Install"
  nsDialogs::Show
FunctionEnd

Function ConfirmLeave
FunctionEnd

Function DirPre
  ${If} $NppFound == "1"
    Abort
  ${EndIf}
FunctionEnd

Function DirLeave
  Call ValidateInstalledDir
  ${If} $NppFound != "1"
    ${If} $Candidate32 == "1"
      MessageBox MB_OK|MB_ICONEXCLAMATION "That copy of Notepad++ is 32-bit. Choose the folder that contains 64-bit notepad++.exe."
    ${Else}
      MessageBox MB_OK|MB_ICONEXCLAMATION "That folder does not contain notepad++.exe.$\r$\n$\r$\nChoose the folder where 64-bit Notepad++ is installed."
    ${EndIf}
    Abort
  ${EndIf}
  StrCpy $WriteTestDir "$INSTDIR\plugins\NppSync"
  Call TestWritable
  ${If} $DirWritable != "1"
    Call RelaunchElevated
    ${If} $DirWritable != "1"
      MessageBox MB_ICONSTOP "Windows did not allow installing into$\r$\n$INSTDIR\plugins\NppSync$\r$\n$\r$\nChoose Yes when Windows asks for permission."
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

Function LaunchNpp
  Exec '"$INSTDIR\notepad++.exe"'
FunctionEnd

Function RegisterUninstall
  ; Per-user Notepad++ lives under LocalAppData, so its uninstall entry stays
  ; in HKCU. An elevated install anywhere else (Program Files, or another
  ; folder that needed the Windows permission prompt) is recorded in HKLM.
  SetRegView 64
  Call IsElevated
  StrCpy $R8 "$INSTDIR\"
  StrLen $R1 "$LOCALAPPDATA\"
  StrCpy $R2 "$R8" $R1
  ${If} $R2 == "$LOCALAPPDATA\"
    SetShellVarContext current
  ${ElseIf} $Elevated == "1"
    SetShellVarContext all
  ${Else}
    SetShellVarContext current
  ${EndIf}

  ClearErrors
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "DisplayName" "Notepad++ Sync"
  ${If} ${Errors}
    SetShellVarContext current
  ${EndIf}
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "DisplayName" "Notepad++ Sync"
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "DisplayVersion" "${VERSION}"
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "Publisher" "Notepad++ Sync"
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "UninstallString" '"$INSTDIR\plugins\NppSync\Uninstall.exe"'
  WriteRegStr SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "InstallLocation" "$INSTDIR\plugins\NppSync"
  WriteRegDWORD SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "NoModify" 1
  WriteRegDWORD SHCTX "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync" "NoRepair" 1
FunctionEnd

Section "Notepad++ Sync"
  Call ValidateInstalledDir
  ${If} $NppFound != "1"
    MessageBox MB_ICONSTOP "64-bit Notepad++ was not found in$\r$\n$INSTDIR"
    SetErrorLevel 2
    Quit
  ${EndIf}

  Call EnsureNppClosed

  DetailPrint "Installing into $INSTDIR\plugins\NppSync"
  DetailPrint "Server address, sign-in, and encryption settings are not changed."

  SetOverwrite on
  CreateDirectory "$INSTDIR\plugins\NppSync"
  SetOutPath "$INSTDIR\plugins\NppSync"
  File "/oname=NppSync.dll" "${PLUGIN_DLL}"
  File "/oname=LICENSE.txt" "${LICENSE_FILE}"
!ifdef INCLUDE_DEPS
  CreateDirectory "$INSTDIR\plugins\NppSync\deps"
  SetOutPath "$INSTDIR\plugins\NppSync\deps"
  File /r "${PLUGIN_DEPS}\*.*"
  SetOutPath "$INSTDIR\plugins\NppSync"
!endif
  WriteUninstaller "$INSTDIR\plugins\NppSync\Uninstall.exe"
  Call RegisterUninstall
SectionEnd

Section "Uninstall"
  Call un.EnsureNppClosed
  DetailPrint "Removing the plugin. %APPDATA%\Notepad++Sync is left in place."
  Delete "$INSTDIR\NppSync.dll"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\README.txt"
  Delete "$INSTDIR\.__npsync_write_test"
  RMDir /r "$INSTDIR\deps"
  Delete "$INSTDIR\Uninstall.exe"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
  SetRegView 64
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\NppSync"
SectionEnd
