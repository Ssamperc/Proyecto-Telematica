CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
LDFLAGS = -pthread

SRC_DIR = src
BIN_DIR = bin

PIBL_SRC = $(SRC_DIR)/pibl.c
TWS_SRC  = $(SRC_DIR)/tws.c

PIBL_BIN = $(BIN_DIR)/pibl
TWS_BIN  = $(BIN_DIR)/tws

.PHONY: all clean dirs

all: dirs $(PIBL_BIN) $(TWS_BIN)

dirs:
	@mkdir -p $(BIN_DIR) logs cache www/static

$(PIBL_BIN): $(PIBL_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[OK] Compilado: $@"

$(TWS_BIN): $(TWS_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)
	@echo "[OK] Compilado: $@"

clean:
	rm -rf $(BIN_DIR)
	@echo "[OK] Limpiado"
