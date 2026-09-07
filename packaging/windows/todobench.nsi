!include "MUI2.nsh"

!ifndef VERSION
  !define VERSION "0.1.0"
!endif
!ifndef ARCH
  !define ARCH "x64"
!endif
!ifndef STAGE
  !error "STAGE must be defined to the staged install directory"
!endif

!define MUI_ICON "${__FILEDIR__}\..\icons\todobench.ico"
!define MUI_UNICON "${__FILEDIR__}\..\icons\todobench.ico"
Name "TodoBench"
!ifndef OUTPUT_FILE
  !error "OUTPUT_FILE is required"
!endif
OutFile "${OUTPUT_FILE}"
InstallDir "$LOCALAPPDATA\TodoBench"
RequestExecutionLevel user
SetCompressor lzma

!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${STAGE}\*.*"
  CreateShortCut "$SMPROGRAMS\TodoBench.lnk" "$INSTDIR\TodoBench.exe"
  WriteUninstaller "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TodoBench" "DisplayName" "TodoBench"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TodoBench" "UninstallString" "$INSTDIR\Uninstall.exe"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TodoBench" "DisplayVersion" "${VERSION}"
SectionEnd

Section "Uninstall"
  Delete "$SMPROGRAMS\TodoBench.lnk"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\TodoBench"
  RMDir /r "$INSTDIR"
SectionEnd
