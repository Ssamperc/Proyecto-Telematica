#!/bin/bash
# Genera los recursos de prueba para los casos 3 y 4
WWW="./www"
STATIC="$WWW/static"

mkdir -p "$STATIC"

echo "[*] Generando archivo ~1MB para caso 3..."
dd if=/dev/urandom bs=1024 count=1024 2>/dev/null | base64 | head -c 1048576 > "$STATIC/large_file.bin"
echo "[OK] $STATIC/large_file.bin"

echo "[*] Generando múltiples archivos ~1MB total para caso 4..."
for i in 1 2 3 4 5; do
    dd if=/dev/urandom bs=1024 count=200 2>/dev/null | base64 | head -c 204800 > "$STATIC/file_part${i}.bin"
    echo "[OK] $STATIC/file_part${i}.bin"
done

echo "[*] Generando imágenes placeholder (PNG mínimo)..."
# PNG 1x1 pixel blanco en base64
PNG_B64="iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="
for name in sample img1 img2 img3 img4 img5; do
    echo "$PNG_B64" | base64 -d > "$STATIC/${name}.png"
    echo "[OK] $STATIC/${name}.png"
done

echo ""
echo "[LISTO] Recursos generados en $WWW/"
ls -lh "$STATIC/"
