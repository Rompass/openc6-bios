BUILD_DIR ?= build

.PHONY: all clean payloads loader zc6_pack

all:
	cmake -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR)

payloads:
	cmake -B $(BUILD_DIR)
	cmake --build $(BUILD_DIR) --target payloads

clean:
	rm -rf $(BUILD_DIR)
