# Cliente FTP concurrente (Proyecto)

**Resumen**  
Este repositorio contiene la implementación de un **cliente FTP concurrente** en C que usa el protocolo definido en el RFC 959. El cliente soporta los comandos básicos (`USER`, `PASS`, `STOR`, `RETR`, `PORT`, `PASV`) y comandos adicionales (`MKD`, `PWD`, `DELE`, `REST`). También implementa transferencia concurrente mediante procesos (`mget`). Los archivos auxiliares `connectsock.c`, `connectTCP.c`, `passivesock.c`, `passiveTCP.c`, `errexit.c` se incluyen para la gestión de sockets.

---

## Estructura del repositorio
├── Makefile
├── TCPftp.c
├── connectsock.c
├── connectTCP.c
├── passivesock.c
├── passiveTCP.c
├── errexit.c
├── scripts/
│ ├── actualizar_portproxy_ftp.ps1
├── tests/
│ ├── archivoPruebaftp1.c
│ └── ...
└── .gitignore



- `TCPftp.c`: cliente FTP (principal).
- `connectsock.c`, `connectTCP.c`, `passivesock.c`, `passiveTCP.c`, `errexit.c`: utilidades de sockets.
- `Makefile`: compilar todo.
- `scripts/`: scripts PowerShell para gestionar `netsh portproxy` (Windows ⇄ WSL).
- `tests/`: archivos de prueba (opcional).
- No subir binarios (`TCPftp`, `*.o`) ni contraseñas.

---

## Requisitos

- WSL2 (Ubuntu 20.04/22.04/24.04 o similar) o Linux con `vsftpd`.
- En Windows: PowerShell (ejecutar como Administrador para scripts `netsh`).
- GCC (`gcc`) y `make`.
- Conexión de red local entre Windows y WSL (WSL2 usa NAT por defecto).

---

## Configuración e instalación del servidor (WSL)

1. **Instalar vsftpd (en WSL):**
```bash
sudo apt update
sudo apt install -y vsftpd
```

2. **Copiar / editar la configuración**
Reemplaza /etc/vsftpd.conf por la siguiente configuración utilizada en el proyecto:

```conf
# vsftpd minimal config for testing in WSL2
listen=YES
listen_ipv6=NO

anonymous_enable=NO
local_enable=YES
write_enable=YES

dirmessage_enable=YES
use_localtime=YES

xferlog_enable=YES
#connect_from_port_20=YES

# Chroot local users to their home
chroot_local_user=YES
# If you get errors related to writable chroot, enable the next line (see notes)
allow_writeable_chroot=YES

# PASV (passive) mode - server will allocate data ports from this range
pasv_enable=YES
pasv_min_port=50000
pasv_max_port=50010
pasv_promiscuous=YES
# pasv_address=TU_IP_PUBLICA_O_HOSTNAME   # descomenta solo si estás exponiendo a Internet/NAT y conoces la IP

# Secure chroot dir
secure_chroot_dir=/var/run/vsftpd/empty

pam_service_name=vsftpd

# TLS disabled for now (disable if you don't have keys)
ssl_enable=NO

# Optional: tune logging (defaults are usually fine)
# xferlog_std_format=YES

# Logging
vsftpd_log_file=/var/log/vsftpd.log
log_ftp_protocol=YES
xferlog_enable=YES
debug_ssl=NO

# para que los procesos no se vean como /usr/sbin/vsftpd
setproctitle_enable=YES

# activacion de PORT
port_enable=YES
port_promiscuous=YES ```

Nota: durante el desarrollo comenté connect_from_port_20=YES porque en WSL/vsftpd en modo activo (PORT) a veces produce vsf_sysutil_bind. Si ves errores 500 OOPS: vsf_sysutil_bind, comentar esa línea suele resolverlo.

3. **Crear un usuario de prueba (ejemplo)**
```
sudo adduser ftpuser
# iniciar el servidor vsftpd
sudo service vsftpd start
# verificar logs y que vsftpd esté escuchando
sudo ss -tulpn | grep :21
tail -f /var/log/vsftpd.log
```

Si se va a conectar desde Windows (FileZilla, cmd, etc.) hacia WSL, la IP de WSL cambia con frecuencia (normalmente después de cada reinicio del ordenador). El scripts scripts/actualizar_portproxy_ftp.ps1 automatizan la creación / eliminación de la regla de netsh que reenvía el puerto 21 en Windows hacia la IP de WSL.

Es necesario:
- Ejecutar como administrador PowerShell.
- Ejecutar el comando ```Set-ExecutionPolicy Bypass -Scope Process -Force``` si no permite scripts.
- Ejecuta el script ```.\actualizar_portproxy_ftp.ps1```


## Uso del Cliente
1. **Compilar el cliente**
En WSL se ubica en el directorio de trabajo, ejecutar el Makefile de la siguiente manera
```make clean 
make ```

2. **Ejecutar el cliente**
Hay que apuntar al servidor local.
```./TCPftp localhost 21```

**Ejemplo de sesión**
```USER: ftpuser
PASS: tupassword

ftp> help
# verás la lista de comandos
ftp> dir
ftp> get archivoRemoto.txt
ftp> put archivoLocal.txt
ftp> pput archivoLocal.txt   # modo activo (PORT)
ftp> mget f1 f2 f3           # descarga varios archivos en paralelo (forks)
ftp> mkd nuevodir
ftp> pwd
ftp> dele antiguo.txt
ftp> rest 100
ftp> get archivoGrande.bin    # reanuda desde byte 100 (si el servidor lo permite en binario)
ftp> quit```
