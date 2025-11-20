# Ejecutar como Administrador
# Uso:
# Set-ExecutionPolicy Bypass -Scope Process -Force
# .\actualizar_portproxy_ftp.ps1

# Mostrar un mensaje indicando que se va a obtener la IP de WSL
Write-Host "Obteniendo IP actual de WSL..."

# Ejecutar WSL para obtener la IP y tomar solo la primera dirección
$wsl_ip = (wsl hostname -I).Split(" ")[0]

# Mostrar la IP detectada en WSL
Write-Host "IP detectada en WSL: $wsl_ip"

# Mensaje indicando que se eliminará la regla anterior del puerto 21 si existe
Write-Host "Eliminando regla anterior del puerto 21 (si existe)..."

# Eliminar la regla existente de portproxy en el puerto 21
# Se usa Out-Null para que no se muestre salida en la consola
netsh interface portproxy delete v4tov4 listenport=21 | Out-Null

# Mensaje indicando que se va a crear una nueva regla de portproxy
Write-Host "Creando nueva regla del puerto 21..."

# Crear la nueva regla de portproxy para redirigir el puerto 21 de localhost a la IP de WSL
netsh interface portproxy add v4tov4 listenport=21 connectaddress=$wsl_ip connectport=21

# Mensaje final indicando que la actualización de portproxy fue exitosa
Write-Host "`nPortproxy FTP actualizado correctamente."
Write-Host "   localhost:21 --> $wsl_ip:21"
