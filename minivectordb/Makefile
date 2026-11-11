# MiniVectorDB 本地构建。
#
# 用法：
#   make -C minivectordb            # 或 cd minivectordb && make
#   make -C minivectordb clean
#
# 产物：minivectordb/build/minivectordb
# 目标文件：minivectordb/build/obj/

CC ?= cc

# 任务要求的编译选项。CFLAGS 用 override 追加，保证即使外部设置了 CFLAGS，
# 这四项也一定在命令行里生效。
BASE_CFLAGS := -std=c11 -O3 -Wall -Wextra
# -std=c11 会隐藏 POSIX 声明（fsync/mkdir/ftruncate/fileno 等），
# 不显式开启这些特性宏，实现方代码会出现隐式声明告警甚至编译失败。
FEATURE_DEFS := -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64

CFLAGS ?=
override CFLAGS += $(BASE_CFLAGS) $(FEATURE_DEFS)
CPPFLAGS ?=
override CPPFLAGS += -Iinclude
LDFLAGS ?=
LDLIBS ?= -lm

SRCDIR    := src
BUILDDIR  := build
OBJDIR    := $(BUILDDIR)/obj
SRCS      := $(wildcard $(SRCDIR)/*.c)
OBJS      := $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SRCS))
DEPS      := $(OBJS:.o=.d)

# Windows（MSYS/MinGW）下产物需要 .exe 后缀。
ifeq ($(OS),Windows_NT)
TARGET := $(BUILDDIR)/minivectordb.exe
else
TARGET := $(BUILDDIR)/minivectordb
endif

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJS)
	@mkdir -p $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(OBJDIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -rf $(BUILDDIR)

-include $(DEPS)
