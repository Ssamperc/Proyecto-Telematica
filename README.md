# Proyecto 1 — PIBL-WS: Proxy Inverso + Balanceador de Carga + Web Server

**Curso:** Telemática/Internet: Arquitectura y Protocolos  
**Lenguaje:** C (estándar POSIX)  
**Protocolo:** HTTP/1.1 (RFC 2616)

---

## 1. Introducción

Este proyecto implementa desde cero, utilizando la **API Sockets de C**, dos componentes fundamentales de una arquitectura web distribuida:

1. **TWS (Telematics Web Server):** servidor HTTP/1.1 con soporte para los métodos GET, HEAD y POST, manejo concurrente de clientes mediante hilos POSIX y sistema de logging.
2. **PIBL (Proxy Inverso + Balanceador de Carga):** proxy HTTP que distribuye las peticiones de los clientes hacia un conjunto de servidores backend usando la política **Round Robin**, con caché persistente en disco con TTL configurable y registro completo de eventos.

La arquitectura desplegada sigue el esquema:

```
Cliente (browser/curl/postman)
        │
        ▼  :8080
  ┌──────────────┐
  │   PIBL        │  ← Round Robin + Caché + Log
  └──────┬───────┘
         │
   ┌─────┼─────┐
   ▼     ▼     ▼
 TWS1  TWS2  TWS3     (puertos 9001, 9002, 9003)
 :9001 :9002 :9003
```

---

## 2. Desarrollo

### 2.1 Estructura del proyecto

```
pibl-ws/
├── src/
│   ├── pibl.c                 # Proxy Inverso + Balanceador de Carga
│   └── tws.c                  # Telematics Web Server
├── config/
│   └── pibl.conf              # Configuración del PIBL
├── www/                       # Document root del TWS
│   ├── index.html
│   ├── page2.html
│   └── static/                # Recursos estáticos
├── logs/                      # Archivos de log (generado en runtime)
├── cache/                     # Caché en disco (generado en runtime)
├── bin/                       # Binarios compilados (generado por make)
├── Makefile
├── start.sh
├── stop.sh
├── generate_test_resources.sh
└── README.md
```

### 2.2 TWS — Telematics Web Server (`src/tws.c`)

#### Concurrencia
Se utiliza **Thread-based concurrency** con `pthread_create()`. Cada conexión entrante genera un hilo independiente que maneja todo el ciclo de vida de la petición HTTP. Los hilos son *detached* para liberar recursos automáticamente al terminar.

#### Métodos soportados
| Método | Comportamiento |
|--------|---------------|
| GET    | Sirve el archivo desde `DocumentRootFolder`. Incluye cabeceras `Content-Type`, `Content-Length`, `Last-Modified`. |
| HEAD   | Idéntico a GET pero sin cuerpo en la respuesta. |
| POST   | Recibe datos en el body y responde con un JSON de confirmación. |

#### Códigos de respuesta
- `200 OK` — Recurso encontrado y entregado.
- `400 Bad Request` — Petición malformada o método no soportado.
- `404 Not Found` — El recurso no existe en el DocumentRootFolder.

#### MIME types
El servidor detecta el tipo de archivo por extensión y asigna el `Content-Type` correcto: `text/html`, `image/png`, `image/jpeg`, `application/javascript`, `text/css`, entre otros.

#### Logger
Cada petición se registra simultáneamente en **stdout** y en el archivo de log especificado al lanzar el servidor. El formato es:

```
[YYYY-MM-DD HH:MM:SS] IP_CLIENTE "MÉTODO URI" STATUS BYTES
```

#### Ejecución
```bash
./bin/tws <PORT> <LogFile> <DocumentRootFolder>

# Ejemplo:
./bin/tws 9001 ./logs/tws_9001.log ./www
```

### 2.3 PIBL — Proxy Inverso + Balanceador de Carga (`src/pibl.c`)

#### Flujo de una petición
```
1. Acepta conexión TCP del cliente
2. Lee la petición HTTP completa
3. Si método=GET: verifica caché en disco
   3a. HIT + TTL válido → sirve desde caché directamente
   3b. MISS o expirado → continúa al paso 4
4. Selecciona backend con Round Robin (thread-safe con mutex)
5. Abre nuevo socket TCP al backend seleccionado
6. Reenvía la petición (reescribiendo cabecera Host)
7. Lee la respuesta completa del backend
8. Reenvía la respuesta al cliente
9. Si status=200 y método=GET: guarda respuesta en disco (caché)
10. Registra en log: cliente, método, URI, backend, status, bytes, fuente
```

#### Round Robin
La selección de backend se realiza con un contador global protegido por `pthread_mutex_t`. Cada hilo incrementa atómicamente el contador y obtiene `backends[counter % total_backends]`.

#### Caché con TTL
- Cada recurso GET se almacena en `./cache/` con un nombre derivado de la URI.
- Al recibir una petición, se verifica si existe el archivo en caché y si su `mtime` está dentro del TTL.
- El TTL se pasa como argumento al lanzar el PIBL: `./bin/pibl config/pibl.conf 120` (120 segundos).
- Si el backend devuelve un error (no 200), el archivo de caché se elimina.
- La caché **persiste en disco**, por lo que sobrevive reinicios del PIBL.

#### Archivo de configuración (`config/pibl.conf`)
```ini
port     = 8080
log_file = ./logs/pibl.log
backend  = 127.0.0.1:9001
backend  = 127.0.0.1:9002
backend  = 127.0.0.1:9003
```

#### Logger
Formato del log:
```
[YYYY-MM-DD HH:MM:SS] IP_CLIENTE "MÉTODO URI" -> HOST:PUERTO | HTTP STATUS | BYTES bytes | [FUENTE]
```
Donde `FUENTE` es `CACHE-HIT` cuando se sirvió desde caché, o `BACKEND` cuando se consultó un servidor.

#### Ejecución
```bash
./bin/pibl <config_file> [cache_ttl_segundos]

# Ejemplo con TTL de 2 minutos:
./bin/pibl config/pibl.conf 120
```

### 2.4 Compilación

Requiere: `gcc`, `make`, `libpthread` (incluida en Linux).

```bash
make all
```

Esto genera `bin/pibl` y `bin/tws`.

### 2.5 Despliegue en AWS EC2

La arquitectura mínima en AWS requiere **4 instancias EC2** (o 1 instancia con todos los procesos para pruebas):

| Instancia | Rol | Puerto |
|-----------|-----|--------|
| EC2-1 | PIBL (Proxy + Load Balancer) | 8080 (público) |
| EC2-2 | TWS Backend 1 | 9001 (privado) |
| EC2-3 | TWS Backend 2 | 9001 (privado) |
| EC2-4 | TWS Backend 3 | 9001 (privado) |

**Configuración Security Groups:**
- EC2-1: inbound TCP 8080 desde 0.0.0.0/0
- EC2-2/3/4: inbound TCP 9001 solo desde IP privada de EC2-1

**Actualizar `config/pibl.conf`** con las IPs privadas de los backends:
```ini
backend = 10.0.1.10:9001
backend = 10.0.1.11:9001
backend = 10.0.1.12:9001
```

### 2.6 Casos de prueba

Generar recursos de prueba primero:
```bash
chmod +x generate_test_resources.sh
./generate_test_resources.sh
```

#### Caso 1 — Página con hipertextos e imagen
```bash
curl http://localhost:8080/
curl http://localhost:8080/index.html
```

#### Caso 2 — Página con múltiples imágenes
```bash
curl http://localhost:8080/page2.html
```

#### Caso 3 — Archivo ~1MB
```bash
curl http://localhost:8080/static/large_file.bin -o /dev/null -w "%{size_download} bytes\n"
```

#### Caso 4 — Múltiples archivos ~1MB total
```bash
for i in 1 2 3 4 5; do
  curl http://localhost:8080/static/file_part${i}.bin -o /dev/null -s
done
```

#### Verificar Round Robin
```bash
# Ver en el log del PIBL cuál backend atiende cada petición
for i in $(seq 1 9); do curl -s http://localhost:8080/ > /dev/null; done
tail -9 logs/pibl.log
```

#### Prueba de caché
```bash
# Primera petición → BACKEND
curl -s http://localhost:8080/index.html > /dev/null
# Segunda petición → CACHE-HIT
curl -s http://localhost:8080/index.html > /dev/null
tail -2 logs/pibl.log
```

#### Prueba con telnet
```bash
telnet localhost 8080
GET /index.html HTTP/1.1
Host: localhost

```

#### Prueba POST
```bash
curl -X POST http://localhost:8080/api/data \
     -H "Content-Type: application/json" \
     -d '{"nombre":"prueba","valor":42}'
```

---

## 3. Conclusiones

- La implementación demuestra que es posible construir un proxy inverso y un servidor web funcionales usando únicamente la **API Sockets POSIX** en C, sin librerías de red externas.
- El modelo **Thread-per-connection** es sencillo de implementar y adecuado para el nivel de carga de una práctica académica. Para producción se evaluaría un modelo event-driven (epoll) para mayor escalabilidad.
- La caché en disco con TTL configurable reduce significativamente la carga sobre los backends para recursos estáticos frecuentes, y además garantiza disponibilidad ante reinicios del proxy.
- El balanceo **Round Robin** distribuye uniformemente la carga cuando todos los backends tienen capacidad similar. Una mejora futura sería implementar *weighted Round Robin* o *least-connections*.
- El uso de `mutex` para el contador Round Robin y el sistema de logging garantiza **thread-safety** sin degradar el rendimiento.

---

## 4. Referencias

- Beej's Guide to Network Programming: https://beej.us/guide/bgnet/
- RFC 2616 — HTTP/1.1: https://datatracker.ietf.org/doc/rfc2616/
- The Linux Programming Interface — M. Kerrisk, No Starch Press, 2010.
- POSIX Threads Programming — B. Nichols et al., O'Reilly, 1996.
- Material del curso: Telemática/Internet: Arquitectura y Protocolos — EAFIT, 2026.

---

*Versión: 1.0 | Fecha: Marzo 2026*
