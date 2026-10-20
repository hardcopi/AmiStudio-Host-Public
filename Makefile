# AmiStudio Host - Linux build (Windows: see docs/PORTING-WINDOWS.md; macOS later).
CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -pthread
VERSION := $(shell sed -n 's/^\#define HOST_VERSION "\(.*\)"/\1/p' src/host.h)
SRC     = src/hostd.c src/config.c src/auth.c src/util.c src/http.c src/discovery.c src/status.c \
          src/svc_builtin.c src/svc_fetch.c src/svc_proxy.c src/svc_amidrive.c src/plat_posix.c client/ashc.c \
          tools/amidrive_cli.c
HDR     = src/*.h client/ashc.h
LIBS    = -lssl -lcrypto -pthread

all: build/amistudio-hostd build/ash-cli build/services/amidrive/Google\ Drive.info
# amidrive's drawer icon next to the binary, as in the release tarball (services/amidrive/; copied from the AmiDrive release)
build/services/amidrive/Google\ Drive.info: dist/amidrive/Google\ Drive.info
	@mkdir -p build/services/amidrive; cp "dist/amidrive/Google Drive.info" "build/services/amidrive/Google Drive.info"
build/amistudio-hostd: $(SRC) $(HDR)
	@mkdir -p build; $(CC) $(CFLAGS) -DASH_HOSTD -o $@ $(SRC) $(LIBS)
build/ash-cli: tools/ash-cli.c tools/amidrive_cli.c client/ashc.c client/ashc.h src/ash.h
	@mkdir -p build; $(CC) $(CFLAGS) -o $@ tools/ash-cli.c tools/amidrive_cli.c client/ashc.c
# static binary for the release tarball. 0.1.10: glibc's NSS lookups are wrapped to src/nss_off.c (a static glibc
# program that calls them dlopens the system's NSS modules and their libc.so.6, which crashed on Fedora 44); names go
# to the system's getent (plat_posix.c). test/run_tests.py checks that no NSS symbol is left in the binary.
NSS_WRAP = getaddrinfo freeaddrinfo getnameinfo gethostbyname gethostbyname2 gethostbyaddr gethostbyname_r gethostbyname2_r \
           getservbyname getservbyport getprotobyname getpwnam getpwuid getpwnam_r getpwuid_r getgrnam getgrgid
build/static/amistudio-hostd: $(SRC) $(HDR) src/nss_off.c
	@mkdir -p build/static; $(CC) $(CFLAGS) -DASH_HOSTD -static -o $@ $(SRC) src/nss_off.c $(LIBS) -lz -lzstd -ldl $(foreach f,$(NSS_WRAP),-Wl,--wrap=$(f)) 2>&1 | grep -v "statically linked applications requires" || true; test -x $@
test: all
	python3 test/run_tests.py
# release tarball with AmiSurf's browser engine (asb-chromed + Chromium, ~750 MB unpacked): tools/fetch-engine.sh
engine:
	bash tools/fetch-engine.sh
tarball: build/static/amistudio-hostd build/ash-cli engine
	sh dist/mktarball.sh $(VERSION)
install-user: build/amistudio-hostd
	sh dist/install-user.sh
clean:
	rm -rf build
.PHONY: all test tarball engine install-user clean

# Windows: compile-only check with MinGW-w64 (no Windows OpenSSL here, so the Linux OpenSSL headers stand in and
# nothing is linked). A real build needs OpenSSL for MinGW (e.g. MSYS2 mingw-w64-x86_64-openssl): see docs/PORTING-WINDOWS.md
WINCC   = x86_64-w64-mingw32-gcc
WINSRC  = $(filter-out src/plat_posix.c,$(SRC)) src/plat_win32.c tools/ash-cli.c
win-check:
	@rm -rf build/winsys && mkdir -p build/winsys/openssl build/win && for f in /usr/include/openssl/* /usr/include/x86_64-linux-gnu/openssl/*; do ln -sf $$f build/winsys/openssl/; done
	@set -e; for f in $(WINSRC); do $(WINCC) -std=gnu11 -Wall -Wno-format-truncation -D_WIN32_WINNT=0x0600 -Ibuild/winsys -c -o build/win/$$(basename $$f .c).o $$f; done; echo "win-check: all files compile for Windows"
.PHONY: win-check
