# UMC22 Low Latency — driver ASIO para Behringer UMC22

Driver ASIO para Windows (x64) pensado para la **Behringer UMC22** (chip USB Audio CODEC / PCM2902, que no tiene driver ASIO oficial). Ofrece **baja latencia** en amplis virtuales como *Archetype: Nolly X* y además **mezcla el audio de Windows (YouTube, etc.) en los cascos al mismo tiempo**.

## Por qué existe

| Opción | Problema con la UMC22 |
|---|---|
| FlexASIO / WASAPI compartido | La UMC22 en modo compartido tiene un periodo **fijo de 10 ms** → latencia alta |
| ASIO4ALL / modo exclusivo | Latencia baja, pero **ninguna otra app puede sonar** (YouTube se queda mudo) |
| **UMC22 Low Latency** | Exclusivo a **3 ms** + YouTube mezclado dentro del propio driver |

## Cómo funciona

```
Guitarra ─► UMC22 entrada (WASAPI exclusivo, 3 ms) ─┐
                                                    ├─► Nolly X (ASIO) ─┐
                                                    │                   ▼
Brave/YouTube ─► "High Definition Audio Device" ─► loopback ─► resampler ─► mezcla ─► limitador ─► UMC22 salida (exclusivo) ─► cascos
```

- **Entrada y salida de la UMC22 en WASAPI exclusivo**, event-driven, periodo mínimo del dispositivo (144 frames a 48 kHz = 3 ms). Latencia típica con buffer 144: entrada 3 ms, salida 6 ms.
- **Mezcla de YouTube**: el audio de Windows se envía a otro dispositivo (por defecto la salida de la placa, "High Definition Audio Device"), el driver lo captura en *loopback*, lo remuestrea (Hermite cúbico) con corrección de deriva de reloj suavizada y lo suma a la salida, con fundidos para evitar clics.
- **Cambio automático de salida**: al abrir el driver, si Windows usa la UMC22 como salida predeterminada, la cambia al dispositivo de loopback; al cerrarlo, la restaura.
- **Limitador de pico** (−0,2 dBFS) en lugar de clipping duro, **dither TPDF** a 16 bits.
- Si el host solo envía la guitarra por el canal L, se copia también a R (`monoAuto`).
- Interfaz ASIO implementada a nivel ABI: **no requiere el SDK de Steinberg**.

## Requisitos

- Windows 10/11 x64
- Visual Studio 2022 o posterior con "Desarrollo para el escritorio con C++"
- Behringer UMC22 (u otra interfaz USB Audio Class: ajusta los nombres en `config.ini`)

## Compilar e instalar

```bat
build.bat          :: compila umc22asio.dll y tools\bin\*.exe
install.bat        :: copia a bin\ y registra el driver (pide administrador)
install.bat /u     :: desinstala
```

Cierra el host ASIO (Nolly X, DAW…) antes de instalar/actualizar. Después, en el host, elige **ASIO → "UMC22 Low Latency"**, 48000 Hz, buffer 144.

> Para actualizar sin cerrar el host: Windows permite **renombrar** una DLL cargada. Renombra `bin\umc22asio.dll` y copia la nueva; el host la cargará al reiniciarse (la ruta registrada no cambia).

## Configuración

`%APPDATA%\UMC22ASIO\config.ini` (se abre desde el botón *Control Panel* del driver en el host):

| Clave | Por defecto | Descripción |
|---|---|---|
| `render` / `capture` | `USB Audio CODEC` | Subcadena del nombre del dispositivo de salida / entrada |
| `loopback` | `High Definition Audio Device` | Dispositivo cuyo audio se mezcla en los cascos (vacío = desactivado) |
| `ytVolume` | `1.0` | Volumen de la mezcla de Windows/YouTube (0–2) |
| `ytLatencyMs` | `40` | Colchón del audio de YouTube; súbelo si hay cortes |
| `preferredBuffer` | `144` | Buffer ASIO preferido (frames) |
| `monoAuto` | `1` | Copia L→R si el host solo envía el canal izquierdo |
| `autoDefault` | `1` | Cambia/restaura automáticamente la salida predeterminada de Windows |
| `debugMeter` | `0` | Escribe niveles, fase y actividad del limitador en el log cada 5 s |

Log: `%APPDATA%\UMC22ASIO\log.txt` (rota a `log.old.txt` al pasar de 512 KB).

## Solución de problemas

| Síntoma | Causa / solución |
|---|---|
| "La UMC22 está en uso por otra app" | Alguna app tiene la UMC22 abierta (y no sigue la salida predeterminada). Cámbiala de salida en *Mezclador de volumen* |
| YouTube no suena | Revisa que Brave/Windows salgan por el dispositivo `loopback` y que `log.txt` diga `loopback activo` |
| YouTube suena raro / voces débiles | Desactiva *Mejoras de audio* y *Audio espacial* en el dispositivo de loopback. Si también pasa sin el driver, revisa la clavija de los cascos (TRRS con micro en jack TRS o clavija a medio entrar = cancelación L−R) |
| Cortes en YouTube | Sube `ytLatencyMs` a 60–80 |
| Chasquidos en la guitarra | Sube el buffer en el host (192, 256) |

## Herramientas (`tools/`)

- `probe.cpp` — lista los dispositivos con su formato de mezcla, periodos compartido/exclusivo y formatos exclusivos admitidos.
- `testhost.cpp` — host ASIO mínimo: carga `umc22asio.dll` sin registrarla y cuenta callbacks durante 3 s (`testhost.exe [buffer]`).
- `phasetest.cpp` — reproduce un tono mono en el dispositivo de loopback y mide por detección síncrona el nivel y la fase L/R (detecta efectos de Windows que alteran el estéreo).

## Limitaciones

- Solo 44,1 / 48 kHz y 2 entradas / 2 salidas.
- La UMC22 solo admite 16 bits en modo exclusivo.
- La salida de la placa también reproduce el audio de YouTube (solo se oye si hay algo conectado).
- `IPolicyConfig` (cambio de salida predeterminada) es una interfaz no documentada de Windows.
