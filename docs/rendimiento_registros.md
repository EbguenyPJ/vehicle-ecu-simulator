# Rendimiento del registro por ciclo

## Resultados

Medición del 12 de noviembre de 2026 con **43 008 registros idénticos** por archivo. Los tiempos son medianas de 21 muestras y los intervalos corresponden al mínimo y máximo observados. El tamaño incluye la cabecera del archivo y, en ECUB, la longitud y el CRC de cada registro.

| Formato | Tamaño en bytes | Bytes por registro | Escritura mediana [mín–máx], ms | Lectura mediana [mín–máx], ms |
| --- | ---: | ---: | ---: | ---: |
| CSV v1 | 15 423 183 | 358,61 | 49,454 [45,341–69,327] | 66,400 [64,834–67,339] |
| ECUB v1 | 6 658 056 | 154,81 | 56,258 [53,590–60,494] | 45,419 [45,195–47,063] |

En esta corrida, el binario ocupó **56,83 % menos**. Su lectura mediana tomó **31,60 % menos tiempo** y su escritura mediana tomó **13,76 % más tiempo** que CSV. Los rangos observados de escritura se solapan. La medición agrupa los costos de codificación y E/S, incluida la validación de ambos formatos y el CRC de ECUB. [42 muestras originales en nanosegundos](../benchmarks/results/2026-11-12-macos-m3-logs.csv)

## Datos y método

1. `ecu-log-performance` captura mediante `recordObserver` los 42 ciclos de `faults`. La traza incluye arranque, operación nominal, una omisión de batería, acelerador fuera de rango, caducidad y recuperación de las seis señales, sobretemperatura, estado seguro y apagado.
2. Forma un único vector con 1 024 copias de esa traza. Suma `42 × índice_de_copia` al ciclo del registro, a cada ciclo de recepción presente y al de la causa enclavada. La numeración queda de 1 a 43 008, con la misma edad de cada muestra. Los cambios de estado al inicio de cada copia corresponden a reinicios de la traza original. Ambos formatos reciben el mismo vector, con una muestra por ciclo.
3. Usa `std::chrono::steady_clock`, dos pares de calentamiento y 21 pares medidos. En cada par alterna qué formato va primero. Cada formato se escribe, se cierra y se lee inmediatamente desde su ruta. El tamaño se obtiene del archivo cerrado, en bytes lógicos, fuera del cronómetro.
4. La escritura mide apertura, serialización, validación, vaciado con `finish()` y cierre. La lectura mide apertura, decodificación, validación, comprobación de fin de archivo y cierre. El lector materializa los registros en un vector reservado de antemano. Se comprueban todos los campos de cada lectura contra el original fuera del cronómetro, igual que la simulación, la preparación de datos y la impresión de muestras.
5. El experimento deja actuar la caché del sistema operativo. La lectura tras escribir mide archivos con caché potencialmente caliente. La escritura incluye el vaciado de C++ al sistema operativo y se mide sin sincronización física como `fsync`. Los resultados describen acceso secuencial en este entorno.

Se usó una MacBook Air con Apple M3 de 8 núcleos y 16 GB de RAM, macOS 26.5 (25F71) arm64, APFS local en `/private/tmp`, AppleClang 21.0.0, CMake 4.4.4 y Release con `-O3 -DNDEBUG -Werror`. El tamaño fue constante en las 21 muestras de cada formato. Una comprobación independiente de los archivos resultantes confirmó los ciclos 1–43 008 en ambos y verificó los 43 008 CRC con `zlib.crc32`.

### Reproducción

Desde la raíz del proyecto, con un shell POSIX y un generador CMake de configuración única:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DECU_BUILD_BENCHMARKS=ON
cmake --build build/release --target ecu-log-performance --parallel
work_dir=$(mktemp -d)
./build/release/ecu-log-performance "$work_dir/logs" > "$work_dir/samples.csv"
wc -c "$work_dir/logs/trace.csv" "$work_dir/logs/trace.bin"
```

El ejecutable crea el subdirectorio indicado y conserva ambos archivos de la última muestra. El directorio padre debe existir y el subdirectorio debe ser nuevo. Cada fila de `samples.csv` contiene formato, número de muestra, copias de la traza, registros, bytes, nanosegundos de escritura y nanosegundos de lectura. La prueba `benchmark.logs` usa `--verify` con dos copias de la traza y comprueba la lectura, los tamaños, las filas del informe y el rechazo de un directorio existente.

## Inspección y recuperación

| Aspecto | CSV | ECUB |
| --- | --- | --- |
| Inspección manual | La cabecera de 74 columnas identifica los datos. Puede abrirse como texto o importarse a una hoja de cálculo. | Un visor hexadecimal muestra la firma, versión y bytes. `BinaryReader` recupera las unidades físicas, estados y diagnósticos. |
| Lectura tras un error | Se detiene en la primera fila inválida, informa su número y conserva el último registro entregado. | Se detiene en el primer registro inválido, informa el desplazamiento inicial y conserva el último registro entregado. |
| Alteración de una medición | El lector comprueba la estructura y la coherencia del registro. Una edición que preserve ambas puede leerse como dato válido. | El CRC comprueba la longitud y el contenido. Una modificación sin el CRC correspondiente se rechaza. |
| Fin parcial | Una fila incompleta se rechaza. Una última fila completa puede terminar sin salto de línea. | Un registro parcial se informa como truncamiento. Un corte justo entre registros deja un prefijo válido. |

`head -n 2 "$work_dir/logs/trace.csv"` muestra la cabecera y el primer registro. `od -An -tx1 -N 16 "$work_dir/logs/trace.bin"` muestra la cabecera ECUB y el comienzo del primer registro. Los formatos y sus lectores están documentados por separado. [CSV](registro_csv.md) · [ECUB](registro_binario.md)

Al cambiar la velocidad del segundo registro de 0 a 1 km/h, el lector CSV entregó ambos ciclos. El lector ECUB entregó el primero y señaló CRC incorrecto en el byte 154 al conservar el checksum original del segundo. Al cortar los dos archivos dentro del segundo registro, ambos conservaron el primero y señalaron el error en la línea 3 de CSV y el byte 154 de ECUB.

## Alternativa conceptual con SQLite

Un destino SQLite podría recibir el mismo `CycleRecord` desde el observador y almacenar ejecuciones, ciclos, observaciones y diagnósticos relacionados por claves. Una tabla de ciclos conservaría estado, acción, límite de edad e ID desconocido. Las observaciones se identificarían por ejecución, ciclo y señal, con los indicadores de calidad, frescura, validez y duplicado. Los diagnósticos conservarían su orden mediante un ordinal. La causa enclavada tendría su propio ciclo y diagnóstico. La versión del esquema se gestionaría con migraciones y las claves foráneas se habilitarían en cada conexión. [Claves foráneas](https://www.sqlite.org/foreignkeys.html)

Para mantener todo el rango de `uint64_t`, las claves de ciclo y los demás contadores de 64 bits sin signo podrían guardarse como `BLOB` de ocho bytes en big-endian. SQLite usa `INTEGER` con signo y ordena los `BLOB` por bytes, por lo que la representación de ancho fijo preservaría el orden numérico de las claves. Para conservar los bits exactos de una medición, el valor original podría guardarse en un `BLOB` nullable, con un campo `REAL` derivado para consultas numéricas de muestras finitas. [Tipos y orden de comparación](https://www.sqlite.org/datatype3.html)

SQL permitiría filtrar, por ejemplo, los ciclos con temperatura `MISSING` o contar diagnósticos críticos por ejecución. Un índice por ejecución, señal, resultado y ciclo permitiría acotar búsquedas frecuentes a cambio de espacio y actualizaciones adicionales. El motor se enlazaría como biblioteca embebida y añadiría gestión de conexiones, sentencias preparadas y versiones del esquema. [Planificador de consultas](https://www.sqlite.org/queryplanner.html) · [Arquitectura](https://www.sqlite.org/arch.html)

Una transacción podría confirmar juntos los registros de un lote y sus filas relacionadas. El diario de rollback o WAL recuperaría la consistencia tras una interrupción según la configuración. En modo WAL, los lectores pueden consultar instantáneas mientras otro proceso escribe, con un solo escritor simultáneo. El costo y la durabilidad de la escritura dependerían del tamaño de los lotes y de la sincronización elegida. `PRAGMA integrity_check` serviría para diagnosticar la estructura de la base. SQLite se evalúa aquí de forma conceptual. [Transacciones](https://www.sqlite.org/lang_transaction.html) · [WAL](https://www.sqlite.org/wal.html) · [Integridad y sincronización](https://www.sqlite.org/pragma.html)
