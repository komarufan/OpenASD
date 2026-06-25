CC     = clang
AR     = ar
CFLAGS = --target=x86_64-apple-macosx13.0 \
         -ffreestanding \
         -fno-stack-protector \
         -fno-builtin \
         -mno-red-zone \
         -nostdlib \
         -nostdinc \
         -I../libasd/include \
         -I$(shell clang -print-resource-dir)/include \
         -Os -Wall -Wextra

BUILD = build
TARGET = $(BUILD)/libgui.a

SRCS = gui.c
OBJS = $(addprefix $(BUILD)/, $(SRCS:.c=.o))

all: $(TARGET)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(AR) rcs $@ $(OBJS)

clean:
	rm -rf $(BUILD)
