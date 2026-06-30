.PHONY: all clean

all:
	$(MAKE) -C libasd
	$(MAKE) -C libgui
	$(MAKE) -C bin
	$(MAKE) -C sbin
	$(MAKE) -C sh
	$(MAKE) -C drvmgr
	$(MAKE) -C ws
	$(MAKE) -C dock
	$(MAKE) -C term
	$(MAKE) -C files
	$(MAKE) -C hx

clean:
	@for dir in */; do \
		if [ -f "$$dir/Makefile" ]; then \
			$(MAKE) -C "$$dir" clean; \
		fi; \
	done
