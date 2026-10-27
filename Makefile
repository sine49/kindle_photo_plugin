# ============================================================
# photo_plugins — Kindle 照片轮播工具 Makefile (M1: 构建/部署/KUAL 打包)
# ============================================================
# 目标设备：Kindle 7 / FW 5.12.2.2 (i.MX6SL, softfp)
# 工具链：koxtoolchain arm-kindlepw2-linux-gnueabi
#   !! 别用 arm-linux-gnueabihf / kindlehf：hard-float 需 FW >= 5.16.3，
#   现有 fbink-plugin 就是死在错误工具链带来的新 glibc 符号上。
#
# 宿主机 = Windows + WSL：所有目标都请在 WSL 内执行
#   （仓库挂载于 /mnt/e/trae_source/kindle）。
# 本文件只用标准 make + POSIX sh 语法，无 Windows 路径分隔符。
#
# 用法示例（WSL 内执行）：
#   make host-test                             # 宿主编译并运行单元测试
#   make fbink                                 # 构建 FBInk 静态库 libfbink.a
#   make all                                   # 交叉编译 -> bin/photo-player
#   make CROSS_TC=arm-kindlehf-linux-gnueabihf all   # 换工具链（日后新机型）
#   make install KINDLE_IP=192.168.15.244      # scp 安装到 Kindle
#   make package                               # 产出 KUAL 可拷贝包
#   make check-abi                             # glibc 符号版本审计
#   make clean
#   make help
#
# 变量（全部可用 ?= / 命令行覆盖）：
#   CROSS_TC   交叉工具链前缀，默认 arm-kindlepw2-linux-gnueabi
#   FBINK_DIR  FBInk 源码目录，默认 ../../.slim/clonedeps/repos/NiLuJe__FBInk
#   KINDLE_IP  Kindle IP，install 必填（无默认）
#   OUT_DIR    中间产物目录，默认 build
#   MAX_GLIBC  check-abi 允许的最高 glibc 符号版本，默认 2.17
#   HOSTCC     宿主编译器，默认 cc（可 make HOSTCC=gcc host-test）
# ============================================================

CROSS_TC  ?= arm-kindlepw2-linux-gnueabi
FBINK_DIR ?= ../../.slim/clonedeps/repos/NiLuJe__FBInk
OUT_DIR   ?= build
MAX_GLIBC ?= 2.17
KINDLE_IP ?=

# 设备侧扩展目录
KINDLE_DIR ?= /mnt/us/extensions/photo_plugins

# 交叉工具链
CROSS_CC     := $(CROSS_TC)-gcc
CROSS_STRIP  := $(CROSS_TC)-strip
CROSS_READELF ?= $(CROSS_TC)-readelf

# 宿主编译器（host-test 用）
HOSTCC ?= cc

# FBInk 静态库（FBInk `make static` 产物）及其内置依赖 libi2c.a
# （libi2c 链法沿用 extensions/fbink-plugin/Makefile）
FBINK_LIB := $(FBINK_DIR)/Release/static/libfbink.a
I2C_LIB   := $(FBINK_DIR)/libi2c-staged/lib/libi2c.a

# 编译选项（注意：不定义 FBINK_SHAREDLIB —— 与静态链接矛盾）
CFLAGS   := -std=gnu99 -Os -Wall -Wextra -Isrc -I$(FBINK_DIR)
INCLUDES := -I$(FBINK_DIR)/i2c-tools/include

# 链接：libfbink.a + libi2c.a + -lrt（老 glibc 的 clock_gettime 在 librt）
#       -lpthread -lm
LDLIBS := $(FBINK_LIB) $(I2C_LIB) -lrt -lpthread -lm

# 源码文件名契约（与 src/ 约定一致，勿改名）
SRCS := \
	src/config.c \
	src/source_local.c \
	src/player.c \
	src/display.c \
	src/platform.c \
	src/log.c \
	src/util.c \
	src/touch.c \
	src/main.c

# 宿主单元测试：不含 main.c / display.c / platform.c
HOST_SRCS := \
	src/config.c \
	src/source_local.c \
	src/player.c \
	src/log.c \
	src/util.c

HOST_TEST_SRCS := \
	test/host/test_config.c \
	test/host/test_source_local.c \
	test/host/test_player.c \
	test/host/test_main.c

TARGET := bin/photo-player

.PHONY: all fbink host-test install package check-abi clean help

# ------------------------------------------------------------
# all: 交叉编译 -> bin/photo-player（strip）
# ------------------------------------------------------------
all:
	@if [ ! -f "$(FBINK_LIB)" ]; then \
		echo "libfbink.a not found at $(FBINK_LIB); building FBInk first..."; \
		$(MAKE) fbink; \
	fi
	@mkdir -p bin
	$(CROSS_CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $(TARGET) $(LDLIBS)
	$(CROSS_STRIP) $(TARGET)
	@echo "========================================="
	@echo "Build complete: $(TARGET)"
	@echo "========================================="

# ------------------------------------------------------------
# fbink: 构建 FBInk 静态库（透传 CROSS_TC + KINDLE=true）
# 注意：必须传 KINDLE=true，否则 FBInk 默认回退 -DFBINK_FOR_KOBO，
#       平台风味错误会导致真机设备识别与 eink 刷新 ioctl 失效
#       （fbink-plugin 实验的 Makefile 同样漏了此参数）。
# ------------------------------------------------------------
fbink:
	@echo "========================================="
	@echo "Building FBInk static library (CROSS_TC=$(CROSS_TC), KINDLE=true)..."
	@echo "========================================="
	$(MAKE) -C $(FBINK_DIR) static CROSS_TC=$(CROSS_TC) KINDLE=true

# ------------------------------------------------------------
# host-test: 宿主编译并运行单元测试（测试退出码非 0 则 make 失败）
# ------------------------------------------------------------
host-test:
	@mkdir -p $(OUT_DIR)
	$(HOSTCC) -std=gnu99 -Wall -Wextra -DHOST_TEST -Isrc \
		-o $(OUT_DIR)/host_tests $(HOST_SRCS) $(HOST_TEST_SRCS)
	./$(OUT_DIR)/host_tests

# ------------------------------------------------------------
# install: scp 安装到 Kindle（WSL 内发起）
# ------------------------------------------------------------
install:
	@if [ -z "$(KINDLE_IP)" ]; then \
		echo "Error: KINDLE_IP is required."; \
		echo "Usage: make install KINDLE_IP=192.168.x.x"; \
		exit 1; \
	fi
	@if [ ! -f "$(TARGET)" ]; then \
		echo "Error: $(TARGET) not found. Run 'make all' first."; \
		exit 1; \
	fi
	@echo "Installing to Kindle at $(KINDLE_IP)..."
	ssh root@$(KINDLE_IP) "mkdir -p $(KINDLE_DIR)/bin $(KINDLE_DIR)/config"
	scp config.xml menu.json root@$(KINDLE_IP):$(KINDLE_DIR)/
	scp $(TARGET) bin/photo-player.sh root@$(KINDLE_IP):$(KINDLE_DIR)/bin/
	scp config/photo-player.conf root@$(KINDLE_IP):$(KINDLE_DIR)/config/
	ssh root@$(KINDLE_IP) "chmod +x $(KINDLE_DIR)/bin/*"
	@echo "Install complete"

# ------------------------------------------------------------
# package: 产出 KUAL 可拷贝包 photo_plugins-release/photo_plugins/
# ------------------------------------------------------------
package:
	@if [ ! -f "$(TARGET)" ]; then \
		echo "Error: $(TARGET) not found. Run 'make all' first."; \
		exit 1; \
	fi
	rm -rf photo_plugins-release
	mkdir -p photo_plugins-release/photo_plugins/bin
	mkdir -p photo_plugins-release/photo_plugins/config
	cp config.xml menu.json photo_plugins-release/photo_plugins/
	cp $(TARGET) bin/photo-player.sh photo_plugins-release/photo_plugins/bin/
	cp config/photo-player.conf photo_plugins-release/photo_plugins/config/
	chmod +x photo_plugins-release/photo_plugins/bin/*
	@echo "========================================="
	@echo "Package ready: photo_plugins-release/photo_plugins/"
	@echo "========================================="

# ------------------------------------------------------------
# check-abi: 审计 glibc 符号版本（不得高于 GLIBC_$(MAX_GLIBC)）
# ------------------------------------------------------------
check-abi:
	@echo "=== check-abi: glibc symbol audit (max allowed: GLIBC_$(MAX_GLIBC)) ==="
	@if [ ! -f "$(TARGET)" ]; then \
		echo "Error: $(TARGET) not found. Run 'make all' first."; \
		exit 1; \
	fi
	@READELF="$(CROSS_READELF)"; \
	if ! command -v "$$READELF" >/dev/null 2>&1; then READELF="readelf"; fi; \
	echo "readelf: $$READELF"; \
	mkdir -p $(OUT_DIR); \
	"$$READELF" --version-info $(TARGET) > $(OUT_DIR)/abi-report.txt; \
	echo "--- GLIBC versions referenced ---"; \
	sed -n 's/.*\(GLIBC_[0-9][0-9.]*\).*/\1/p' $(OUT_DIR)/abi-report.txt \
		| sort -u > $(OUT_DIR)/abi-versions.txt; \
	cat $(OUT_DIR)/abi-versions.txt; \
	echo "-----------------------------------"; \
	BAD=0; \
	for v in `cat $(OUT_DIR)/abi-versions.txt`; do \
		ver="$${v#GLIBC_}"; \
		if awk -v a="$$ver" -v b="$(MAX_GLIBC)" 'BEGIN { na=split(a,A,"."); nb=split(b,B,"."); for (i=1;i<=3;i++) { x=(i<=na?A[i]+0:0); y=(i<=nb?B[i]+0:0); if (x>y) exit 1; if (x<y) exit 0 } exit 0 }'; then \
			echo "  OK   $$v"; \
		else \
			echo "  FAIL $$v > GLIBC_$(MAX_GLIBC)"; \
			BAD=1; \
		fi; \
	done; \
	if [ "$$BAD" -ne 0 ]; then \
		echo "check-abi FAILED: $(TARGET) needs glibc newer than GLIBC_$(MAX_GLIBC)"; \
		echo "(target device: Kindle 7 / FW 5.12.2.2 — use the koxtoolchain, not gnueabihf!)"; \
		exit 1; \
	fi; \
	echo "check-abi PASSED: all symbol versions <= GLIBC_$(MAX_GLIBC)"

# ------------------------------------------------------------
# clean
# ------------------------------------------------------------
clean:
	rm -rf $(OUT_DIR) $(TARGET) photo_plugins-release
	@echo "Clean complete"

# ------------------------------------------------------------
# help
# ------------------------------------------------------------
help:
	@echo "photo_plugins Makefile — Kindle 照片轮播工具 (Kindle 7 / FW 5.12.2.2)"
	@echo ""
	@echo "Targets:"
	@echo "  all        - 交叉编译 bin/photo-player（strip；缺 libfbink.a 时先建 FBInk）"
	@echo "  fbink      - 构建 FBInk 静态库 (make -C \$$FBINK_DIR static)"
	@echo "  host-test  - 宿主编译并运行单元测试 -> \$$OUT_DIR/host_tests"
	@echo "  install    - scp 安装到 Kindle（需 KINDLE_IP=...）"
	@echo "  package    - 产出 photo_plugins-release/photo_plugins/（KUAL 可拷贝包）"
	@echo "  check-abi  - glibc 符号版本审计（上限 MAX_GLIBC）"
	@echo "  clean      - 删除 \$$OUT_DIR/、bin/photo-player、photo_plugins-release/"
	@echo "  help       - 显示本帮助"
	@echo ""
	@echo "Variables (all overridable):"
	@echo "  CROSS_TC=$(CROSS_TC)"
	@echo "  FBINK_DIR=$(FBINK_DIR)"
	@echo "  OUT_DIR=$(OUT_DIR)  MAX_GLIBC=$(MAX_GLIBC)  HOSTCC=$(HOSTCC)"
	@echo "  KINDLE_IP=$(KINDLE_IP)"
	@echo ""
	@echo "Examples (run inside WSL):"
	@echo "  make host-test"
	@echo "  make all"
	@echo "  make CROSS_TC=arm-kindlehf-linux-gnueabihf all"
	@echo "  make install KINDLE_IP=192.168.15.244"
	@echo "  make package"
	@echo "  make check-abi"
