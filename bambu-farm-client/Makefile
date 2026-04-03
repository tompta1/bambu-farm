.ONESHELL:

BAMBU_STUDIO_PLUGIN_DIR ?= $(if $(XDG_CONFIG_HOME),$(XDG_CONFIG_HOME),$(HOME)/.config)/BambuStudio/plugins
FLATPAK_APP_ID ?= com.bambulab.BambuStudio
FLATPAK_BAMBU_STUDIO_PLUGIN_DIR ?= $(HOME)/.var/app/$(FLATPAK_APP_ID)/config/BambuStudio/plugins
FLATPAK_BAMBU_STUDIO_CONFIG_DIR ?= $(HOME)/.var/app/$(FLATPAK_APP_ID)/config/BambuStudio
SERVER_CONFIG ?= ../bambu-farm-server/bambufarm.toml
SERVER_CONFIG_EXAMPLE ?= ../bambu-farm-server/bambufarm_example.toml
BUILD_DIR ?= target/debug/shared
TEST_BIN ?= target/debug/print_job_test
CLOUD_TEST_BIN ?= target/debug/cloud_compat_test
LOCAL_STATE_TEST_BIN ?= target/debug/local_state_test
SESSION_STATE_TEST_BIN ?= target/debug/session_state_test
CALLBACK_REGISTRY_TEST_BIN ?= target/debug/callback_registry_test
LOCAL_PRINT_CONTEXT_TEST_BIN ?= target/debug/local_print_context_test
PRINTER_METADATA_TEST_BIN ?= target/debug/printer_metadata_test
TUNNEL_BRIDGE_TEST_BIN ?= target/debug/tunnel_bridge_test
PRINT_FLOW_TEST_BIN ?= target/debug/print_flow_test

shared:
	cargo build
	mkdir -p "$(BUILD_DIR)"
	cd "$(BUILD_DIR)"
	rm -f ./* || echo "nothing to remove"
	ar -x ../libbambu_farm_client.a
	gcc -shared -Wl,-z,noexecstack *.o -o libbambu_networking.so
	gcc -fPIC -shared ../../../cpp/bambu_source.c -ldl -o libBambuSource.so

install-host: shared
	mkdir -p "$(BAMBU_STUDIO_PLUGIN_DIR)"
	ln -sfn "$$(pwd)/$(BUILD_DIR)/libbambu_networking.so" "$(BAMBU_STUDIO_PLUGIN_DIR)/libbambu_networking.so"
	ln -sfn "$$(pwd)/$(BUILD_DIR)/libBambuSource.so" "$(BAMBU_STUDIO_PLUGIN_DIR)/libBambuSource.so"
	printf 'Installed plugins into %s\n' "$(BAMBU_STUDIO_PLUGIN_DIR)"

install-flatpak: shared
	mkdir -p "$(FLATPAK_BAMBU_STUDIO_PLUGIN_DIR)"
	cp -f "$$(pwd)/$(BUILD_DIR)/libbambu_networking.so" "$(FLATPAK_BAMBU_STUDIO_PLUGIN_DIR)/libbambu_networking.so"
	cp -f "$$(pwd)/$(BUILD_DIR)/libBambuSource.so" "$(FLATPAK_BAMBU_STUDIO_PLUGIN_DIR)/libBambuSource.so"
	mkdir -p "$(FLATPAK_BAMBU_STUDIO_CONFIG_DIR)"
	if [ -f "$(SERVER_CONFIG)" ]; then \
		cp -f "$(SERVER_CONFIG)" "$(FLATPAK_BAMBU_STUDIO_CONFIG_DIR)/bambufarm.toml"; \
	elif [ -f "$(SERVER_CONFIG_EXAMPLE)" ]; then \
		cp -f "$(SERVER_CONFIG_EXAMPLE)" "$(FLATPAK_BAMBU_STUDIO_CONFIG_DIR)/bambufarm.toml"; \
	fi
	printf 'Installed Flatpak plugins into %s\n' "$(FLATPAK_BAMBU_STUDIO_PLUGIN_DIR)"

shared-flatpak: install-flatpak

test-helpers:
	mkdir -p "$(dir $(TEST_BIN))"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/print_job_test.cpp \
		-o "$(TEST_BIN)"
	"$(TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/cloud_compat.cpp cpp/cloud_compat_test.cpp \
		-o "$(CLOUD_TEST_BIN)"
	"$(CLOUD_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/local_state.cpp cpp/local_state_test.cpp \
		-o "$(LOCAL_STATE_TEST_BIN)"
	"$(LOCAL_STATE_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/session_state.cpp cpp/session_state_test.cpp \
		-o "$(SESSION_STATE_TEST_BIN)"
	"$(SESSION_STATE_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/callback_registry.cpp cpp/callback_registry_test.cpp \
		-o "$(CALLBACK_REGISTRY_TEST_BIN)"
	"$(CALLBACK_REGISTRY_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/local_state.cpp cpp/local_print_context.cpp cpp/local_print_context_test.cpp \
		-o "$(LOCAL_PRINT_CONTEXT_TEST_BIN)"
	"$(LOCAL_PRINT_CONTEXT_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/local_state.cpp cpp/printer_metadata.cpp cpp/printer_metadata_test.cpp \
		-o "$(PRINTER_METADATA_TEST_BIN)"
	"$(PRINTER_METADATA_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/tunnel_protocol.cpp cpp/tunnel_bridge_test.cpp \
		-o "$(TUNNEL_BRIDGE_TEST_BIN)"
	"$(TUNNEL_BRIDGE_TEST_BIN)"
	g++ -std=c++17 -Wall -Wextra -pedantic -Icpp \
		cpp/print_job.cpp cpp/print_flow.cpp cpp/print_flow_test.cpp \
		-o "$(PRINT_FLOW_TEST_BIN)"
	"$(PRINT_FLOW_TEST_BIN)"

test: test-helpers
