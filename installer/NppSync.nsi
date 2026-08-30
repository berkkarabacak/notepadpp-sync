; NppSync.nsi - NSIS installer script for the Notepad++ Sync plugin.
; Optional packaging path; the ZIP from package.ps1 is the primary artifact.
; Build: makensis /DVERSION=1.0.0 NppSync.nsi

!include "MUI2.nsh"

!ifndef VERSION
!define VERSION "1.0.0"
!endif

Name "Notepad++ Sync ${VERSION}"
OutFile "NotepadPlusPlusSync-${VERSION}-setup.exe"
Unicode true
RequestExecutionLevel admin

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_LANGUAGE "English"

; Default: 64-bit Notepad++ plugin directory. The user can change it.
InstallDir "$PROGRAMFILES64\Notepad++\plugins\NppSync"

Section "Install"
    SetOutPath "$INSTDIR"
    File /oname=NppSync.dll "..\plugin\build\Release\NppSync.dll"
    File "..\README.md"
    File "..\LICENSE"
    WriteUninstaller "$INSTDIR\uninstall.exe"
SectionEnd

Section "Uninstall"
    Delete "$INSTDIR\NppSync.dll"
    Delete "$INSTDIR\README.md"
    Delete "$INSTDIR\LICENSE"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
