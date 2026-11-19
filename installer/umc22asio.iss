; Low Latency ASIO for UMC22 - installer (Inno Setup 6)
; Built by package.bat:  ISCC /DAppVersion=1.2.0 installer\umc22asio.iss
; Copyright (C) 2026 Tohzzicklao - SPDX-License-Identifier: GPL-3.0-or-later

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define AppName "Low Latency ASIO for UMC22"

[Setup]
; fixed AppId: upgrades replace the previous installation instead of adding a second one
AppId={{4B1E7C52-2F0A-4D6B-9E31-8C5A7D2F6E19}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Tohzzicklao
AppPublisherURL=https://github.com/Tohzzicklao/umc22-asio
AppSupportURL=https://github.com/Tohzzicklao/umc22-asio/issues
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=umc22-asio-v{#AppVersion}-setup
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
MinVersion=10.0
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; a running ASIO host keeps the DLL loaded: offer to close it (Restart Manager)
CloseApplications=yes
RestartApplications=no
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}

[Files]
; regserver: registers the ASIO driver on install and unregisters it on uninstall
Source: "..\umc22asio.dll"; DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "..\tools\bin\probe.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\bin\testhost.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\tools\bin\phasetest.exe"; DestDir: "{app}\tools"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName} Settings"; Filename: "{sys}\rundll32.exe"; Parameters: """{app}\umc22asio.dll"",ControlPanel"; IconFilename: "{sys}\mmsys.cpl"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{sys}\rundll32.exe"; Parameters: """{app}\umc22asio.dll"",ControlPanel"; Description: "Open the driver settings"; Flags: postinstall nowait skipifsilent unchecked

[Messages]
FinishedLabel=The driver is installed. Close and reopen your ASIO host (DAW, Neural DSP plugin...) and select "{#AppName}" as the audio device.
