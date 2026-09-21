# STM32F103C8T6 footswitch_hid 工程 Makefile (多文件, 仿 demo)

# 工具链
PREFIX = arm-none-eabi-
CC = $(PREFIX)gcc
AS = $(PREFIX)gcc -x assembler-with-cpp
LD = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
OBJDUMP = $(PREFIX)objdump
SIZE = $(PREFIX)size

# 目标芯片
TARGET = footswitch_hid
CPU = cortex-m3

# 源文件
ASM_SRC = src/startup.s
C_SRC = $(wildcard src/*.c) $(wildcard app/*.c) $(wildcard lib/st_usbfs/src/*.c) $(wildcard lib/cmsis/*.c)

# 输出目录
BUILD_DIR = build

# 编译选项
CFLAGS = -mcpu=$(CPU) -mthumb -std=c11
CFLAGS += -Wall -Wextra -Wshadow
CFLAGS += -fno-common -fno-builtin -ffunction-sections -fdata-sections
CFLAGS += -Os
CFLAGS += -g3 -gdwarf-2  # 调试信息
CFLAGS += -Isrc -Iapp -Ilib/st_usbfs/inc -Ilib/cmsis

ASFLAGS = -mcpu=$(CPU) -mthumb
ASFLAGS += -g3 -gdwarf-2

# 链接选项
LDSCRIPT = src/linker.ld
LDFLAGS = -mcpu=$(CPU) -mthumb
LDFLAGS += -T$(LDSCRIPT)
LDFLAGS += -Wl,--gc-sections  # 移除未使用的代码
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map
LDFLAGS += --specs=nano.specs  # 使用 newlib-nano (更小)
LDFLAGS += --specs=nosys.specs # 不使用系统调用
LDFLAGS += -lm  # 链接数学库

# 目标文件 (保留源文件路径: build/app/x.o, build/lib/x.o ...)
OBJ = $(patsubst %.s,$(BUILD_DIR)/%.o,$(ASM_SRC)) $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SRC))
ELF = $(BUILD_DIR)/$(TARGET).elf
HEX = $(BUILD_DIR)/$(TARGET).hex
BIN = $(BUILD_DIR)/$(TARGET).bin

# 默认目标
all: $(ELF) $(HEX) $(BIN) size

# 编译 C 文件 (自动创建子目录)
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# 编译汇编文件
$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -c $< -o $@

# 链接
$(ELF): $(OBJ)
	$(LD) $(LDFLAGS) $(OBJ) -o $@

# 生成 HEX
$(HEX): $(ELF)
	$(OBJCOPY) -O ihex $< $@

# 生成 BIN
$(BIN): $(ELF)
	$(OBJCOPY) -O binary $< $@

# 显示大小信息
size: $(ELF)
	@echo ""
	@echo "=== 固件大小 ==="
	$(SIZE) $(ELF)
	@echo ""
	@echo "=== 输出文件 ==="
	@echo "  ELF: $(ELF)"
	@echo "  HEX: $(HEX)"
	@echo "  BIN: $(BIN)"
	@echo ""

# 反汇编 (调试用)
disasm: $(ELF)
	$(OBJDUMP) -d -S $(ELF) > $(BUILD_DIR)/$(TARGET).dis
	@echo "反汇编已保存到 $(BUILD_DIR)/$(TARGET).dis"

# 清理
clean:
	rm -rf $(BUILD_DIR)

# 烧录/调试工具: pyocd (项目 venv 内, 目标为 STM32F103C8T6)
PYOCD = ../venv/bin/pyocd
TARGET_TYPE = stm32f103c8

# 烧录 (使用 pyocd)
flash: $(BIN)
	$(PYOCD) flash -t $(TARGET_TYPE) -e sector $<

# 主机端描述符测试
test:
	python3 host_test/test_descriptors.py

# 调试 (启动 GDB server)
debug:
	$(PYOCD) gdbserver -t $(TARGET_TYPE)

.PHONY: all clean size disasm flash test debug
