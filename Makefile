# ==============================================================================
# OpenC6 Automated Setup & Installer (Self-Bootstrapping)
# ==============================================================================
.PHONY: setup

setup:
	@if ! command -v gcc >/dev/null 2>&1; then \
		echo "[*] GCC compiler missing. Bootstrapping host build environment..."; \
		if [ -f /etc/arch-release ]; then \
			if [ "$$(id -u)" -eq 0 ]; then pacman -Sy --noconfirm gcc; else sudo pacman -Sy --noconfirm gcc; fi; \
		elif [ -f /etc/debian_version ]; then \
			if [ "$$(id -u)" -eq 0 ]; then apt-get update && apt-get install -y gcc; else sudo apt-get update && sudo apt-get install -y gcc; fi; \
		else \
			echo "[-] Unknown OS. Please install GCC manually."; exit 1; \
		fi; \
	fi
	@mkdir -p tools/installer/build
	@gcc -std=c99 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L \
		tools/installer/main.c -o tools/installer/build/openc6_installer
	@./tools/installer/build/openc6_installer
