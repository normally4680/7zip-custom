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
  $(O)\PasswordVault.obj

APP_OBJS = $(O)\VaultApp.obj
APP_RES  = $(O)\Vault.res

all: lib app test

lib: $(O)\Custom.lib

app: $(OUTDIR)\7zVault.exe

test: $(OUTDIR)\VaultTest.exe

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

$(O)\PasswordVault.obj: Core\PasswordVault.cpp Core\PasswordVault.h Core\VaultCrypto.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\PasswordVault.obj Core\PasswordVault.cpp

# ---------------------------------------------------------- vault manager app

$(O)\VaultApp.obj: UI\VaultApp.cpp UI\resource.h Core\PasswordVault.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultApp.obj UI\VaultApp.cpp

$(O)\Vault.res: UI\Vault.rc UI\resource.h
	@if not exist "$(O)" mkdir "$(O)"
	$(RC_TOOL) /nologo /fo$(O)\Vault.res UI\Vault.rc

$(OUTDIR)\7zVault.exe: $(APP_OBJS) $(APP_RES) $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:WINDOWS /OUT:$@ $(APP_OBJS) $(APP_RES) \
	  $(O)\Custom.lib user32.lib gdi32.lib comctl32.lib comdlg32.lib \
	  shell32.lib ole32.lib bcrypt.lib crypt32.lib

$(O)\VaultTest.obj: Tests\VaultTest.cpp Core\PasswordVault.h
	@if not exist "$(O)" mkdir "$(O)"
	$(CL_TOOL) $(CFLAGS) /Fo$(O)\VaultTest.obj Tests\VaultTest.cpp

$(OUTDIR)\VaultTest.exe: $(O)\VaultTest.obj $(O)\Custom.lib
	@if not exist "$(OUTDIR)" mkdir "$(OUTDIR)"
	$(LINK_TOOL) /nologo /SUBSYSTEM:CONSOLE /OUT:$@ $(O)\VaultTest.obj \
	  $(O)\Custom.lib user32.lib ole32.lib shell32.lib bcrypt.lib crypt32.lib

clean:
	@if exist "$(O)" rd /s /q "$(O)"
	@if exist "$(OUTDIR)\7zVault.exe" del /q "$(OUTDIR)\7zVault.exe"
	@if exist "$(OUTDIR)\VaultTest.exe" del /q "$(OUTDIR)\VaultTest.exe"
