# ============================================================================
#  Custom/ overlay module build (nmake)
#
#  Independent of 7-Zip makefiles: upstream changes never touch this file.
#  Outputs:
#    build\Custom.lib        static lib linked into 7zG.exe / 7zFM.exe
#    ..\_out\x64\7zVault.exe standalone vault manager (our UI layer)
#
#  Flags mirror 7-Zip (/MT /GS- /EHsc /Zc:wchar_t) so that the static
#  CRT matches and memory never crosses a mismatched heap.
# ============================================================================

# NOTE: nmake exports macros as environment variables to child processes.
# Never name a macro LIB / LINK / RC: LIB would shadow the library search path.
CL_TOOL   = cl
RC_TOOL   = rc
LIB_TOOL  = lib
LINK_TOOL = link

!IFNDEF O
O = build
!ENDIF

OUTDIR = ..\_out\x64

# /utf-8: sources contain Chinese comments and literals
CFLAGS = /nologo /c /MT /GS- /EHsc /Zc:wchar_t /O1 /W3 /utf-8 /DUNICODE /D_UNICODE /I.

!IFDEF DEBUG
CFLAGS = /nologo /c /MTd /GS- /EHsc /Zc:wchar_t /Od /Zi /W3 /utf-8 /DUNICODE /D_UNICODE /I.
!ENDIF

LIB_OBJS = \
  $(O)\CustomHooks.obj \
  $(O)\VaultCrypto.obj \
  $(O)\PasswordVault.obj \
  $(O)\ImagePreview.obj

APP_OBJS = $(O)\VaultApp.obj $(O)\VaultDialog.obj
APP_RES  = $(O)\Vault.res

DLL_OBJS = $(O)\VaultUiExports.obj $(O)\VaultDialog.obj

all: lib app dll test probe previewtest

lib: $(O)\Custom.lib

app: $(OUTDIR)\7zVault.exe

dll: $(OUTDIR)\7zVaultUi.dll

test: $(OUTDIR)\VaultTest.exe

probe: $(OUTDIR)\UiProbe.exe

previewtest: $(OUTDIR)\PreviewTest.exe

# ------------------------------------------------------------- static lib

$(O)\Custom.lib: $(LIB_OBJS)
	@if not exist "$(O)" mkdir "$(O)"
	$(LIB_TOOL) /nologo /out:$(O)\Custom.lib $(LIB_OBJS)

$(O)\CustomHooks.obj: Bridge\CustomHooks.cpp Bridge\CustomHooks.h Core\PasswordVault.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\CustomHooks.obj Bridge\CustomHooks.cpp

$(O)\VaultCrypto.obj: Core\VaultCrypto.cpp Core\VaultCrypto.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultCrypto.obj Core\VaultCrypto.cpp

$(O)\ImagePreview.obj: Core\ImagePreview.cpp Core\ImagePreview.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\ImagePreview.obj Core\ImagePreview.cpp

$(O)\PasswordVault.obj: Core\PasswordVault.cpp Core\PasswordVault.h Core\VaultCrypto.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\PasswordVault.obj Core\PasswordVault.cpp

# ---------------------------------------------------------- vault manager app

$(O)\VaultApp.obj: UI\VaultApp.cpp UI\VaultDialog.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultApp.obj UI\VaultApp.cpp

$(O)\VaultDialog.obj: UI\VaultDialog.cpp UI\VaultDialog.h UI\resource.h Core\PasswordVault.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultDialog.obj UI\VaultDialog.cpp

$(O)\VaultUiExports.obj: UI\VaultUiExports.cpp UI\VaultDialog.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultUiExports.obj UI\VaultUiExports.cpp

$(O)\Vault.res: UI\Vault.rc UI\resource.h
	@if not exist "$(O)" mkdir "$(O)"
	$(RC_TOOL) /nologo /fo$(O)\Vault.res UI\Vault.rc

$(OUTDIR)\7zVault.exe: $(APP_OBJS) $(APP_RES) $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:WINDOWS /OUT:$@ $(APP_OBJS) $(APP_RES) \
	  $(O)\Custom.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib \
	  shell32.lib ole32.lib bcrypt.lib crypt32.lib

$(OUTDIR)\7zVaultUi.dll: $(DLL_OBJS) $(APP_RES) $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /DLL /SUBSYSTEM:WINDOWS /OUT:$@ $(DLL_OBJS) $(APP_RES) \
	  $(O)\Custom.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib \
	  shell32.lib ole32.lib bcrypt.lib crypt32.lib

$(O)\VaultTest.obj: Tests\VaultTest.cpp Core\PasswordVault.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultTest.obj Tests\VaultTest.cpp

$(O)\UiProbe.obj: Tests\UiProbe.cpp
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\UiProbe.obj Tests\UiProbe.cpp

$(OUTDIR)\UiProbe.exe: $(O)\UiProbe.obj $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:CONSOLE /OUT:$@ $(O)\UiProbe.obj \
	  $(O)\Custom.lib user32.lib gdi32.lib comctl32.lib shell32.lib ole32.lib \
	  bcrypt.lib crypt32.lib

$(O)\PreviewTest.obj: Tests\PreviewTest.cpp Core\ImagePreview.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\PreviewTest.obj Tests\PreviewTest.cpp

$(OUTDIR)\PreviewTest.exe: $(O)\PreviewTest.obj $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:CONSOLE /OUT:$@ $(O)\PreviewTest.obj \
	  $(O)\Custom.lib user32.lib shell32.lib ole32.lib bcrypt.lib crypt32.lib

$(OUTDIR)\VaultTest.exe: $(O)\VaultTest.obj $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:CONSOLE /OUT:$@ $(O)\VaultTest.obj \
	  $(O)\Custom.lib user32.lib ole32.lib shell32.lib bcrypt.lib crypt32.lib

clean:
	@if exist "$(O)" rd /s /q "$(O)"
	@if exist "$(OUTDIR)\7zVault.exe" del /q "$(OUTDIR)\7zVault.exe"
	@if exist "$(OUTDIR)\7zVaultUi.dll" del /q "$(OUTDIR)\7zVaultUi.dll"
	@if exist "$(OUTDIR)\VaultTest.exe" del /q "$(OUTDIR)\VaultTest.exe"
	@if exist "$(OUTDIR)\UiProbe.exe" del /q "$(OUTDIR)\UiProbe.exe"
	@if exist "$(OUTDIR)\PreviewTest.exe" del /q "$(OUTDIR)\PreviewTest.exe"
