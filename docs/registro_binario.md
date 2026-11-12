# Registro binario

`--binary ARCHIVO` escribe un registro por ciclo a través de `recordObserver` y presenta el Dashboard durante el recorrido. Selecciona `--binary` o `--csv` para cada ejecución. La ruta se abre después de validar los argumentos y preparar el escenario. Si el archivo existe, la escritura lo reemplaza.

```sh
./build/vehicle-ecu-simulator faults --dashboard ascii --binary "recorrido con fallas.bin"
```

`BinaryWriter` y `BinaryReader`, en `ecu/binary.hpp`, usan el mismo `CycleRecord` que el CSV. Los consumidores CMake enlazan `ecu_binary` para acceder a la API. El llamador conserva la propiedad del flujo y abre los archivos con `std::ios::binary`. Comprueba el resultado de `write()` y `finish()`, y el cierre si el flujo es un archivo. `finish()` escribe la cabecera cuando todavía no hay registros y vacía el flujo. El observador detiene el recorrido si el destino rechaza un registro.

```cpp
ecu::BinaryWriter writer(output);
const bool completed = ecu::runScenario(scenario, ecu::recordObserver(writer));
const bool flushed = writer.finish();
// El llamador comprueba completed y flushed, y cierra output si es un archivo.
```

## Formato ECUB versión 1

Cada byte tiene ocho bits. Los campos multibyte se escriben en **little-endian**, con el byte menos significativo primero. `u8`, `u16`, `u32` y `u64` representan enteros sin signo de ese ancho. El formato se construye campo por campo sin relleno.

### Cabecera del archivo

| Desplazamiento en bytes | Tamaño | Campo | Valor |
| --- | --- | --- | --- |
| 0 | 4 | Firma | ASCII `ECUB` (`45 43 55 42` en hexadecimal). |
| 4 | 2 | Versión `u16` | `1`. |
| 6 | 2 | Reservado `u16` | `0`. |

Después de los ocho bytes de cabecera se almacenan cero o más registros. El primer ciclo puede ser 0. Los siguientes ciclos deben ser estrictamente crecientes y pueden tener saltos.

### Envoltura del registro

| Desplazamiento desde el inicio del registro | Tamaño | Campo |
| --- | --- | --- |
| 0 | 2 | Longitud `L` del contenido, `u16` de 38 a 172. |
| 2 | `L` | Contenido. |
| `2 + L` | 4 | CRC del registro, `u32`. |

El **CRC-32/ISO-HDLC** cubre los dos bytes de longitud tal como se escriben, seguidos de los `L` bytes de contenido. El CRC usa el polinomio `0x04C11DB7` (reflejado `0xEDB88320`), valor inicial y XOR final `0xFFFFFFFF`, y reflexión de entrada y salida. El CRC de los bytes ASCII `123456789` es `0xCBF43926`. La cabecera del archivo y el propio CRC quedan fuera de la cobertura. La secuencia termina en el límite del último registro. El formato carece de un conteo global, por lo que eliminar registros completos al final deja un prefijo legible.

### Contenido del registro

| Desplazamiento desde el inicio del contenido | Tipo | Campo |
| --- | --- | --- |
| 0 | `u64` | Ciclo. |
| 8 | `u64` | Límite de antigüedad `maxAgeCycles`. |
| 16 | `u8` | Estado de Control. |
| 17 | `u8` | Acción de Control. |
| 18 | `u8` | Indicadores. El bit 0 indica un ID desconocido, los bits 1 a 6 indican duplicados en orden de catálogo y el bit 7 indica una causa enclavada. |
| 19 | `u8` | Número de diagnósticos actuales, de 0 a 7. |
| 20 | Variable | Seis bloques de señales en orden de catálogo. |
| Tras las señales | 3 bytes por diagnóstico | Diagnósticos actuales en el orden emitido por Control. |
| Tras los diagnósticos | 11 bytes si el bit 7 está activo | Ciclo de la causa enclavada (`u64`) y diagnóstico de esa causa (3 bytes). |

El orden de catálogo fija también los códigos de señal que usan los diagnósticos.

| Código | Señal |
| --- | --- |
| 0 | `speed` |
| 1 | `engine_rpm` |
| 2 | `coolant_temperature` |
| 3 | `throttle_position` |
| 4 | `battery_voltage` |
| 5 | `oil_pressure` |

### Bloque de señal

| Desplazamiento desde el inicio del bloque | Tipo | Campo |
| --- | --- | --- |
| 0 | `u8` | Resultado de Gateway. |
| 1 | `u8` | Frescura. |
| 2 | `u8` | Tipo de valor. `0` indica ausencia, `1` indica IEEE 754 binary64 y `2` indica un código escalado. |
| 3 | `u8` | Calidad, si hay muestra. |
| 4 | `u64` | Ciclo de recepción, si hay muestra. |
| 12 | `u64`, `u16` o `u8` | Valor según el tipo y la señal. |

El tipo 0 termina el bloque tras el tercer byte y corresponde a `MISSING` con `NEVER_RECEIVED`. Una muestra caducada conserva valor, calidad y recepción. Cuando hay muestra, el lector obtiene la edad restando el ciclo de recepción al del registro y valida su coherencia con la frescura y el resultado almacenados.

El tipo 1 conserva los bits del `double` en la unidad física del catálogo, incluidos ceros con signo, infinitos y NaN. El tipo 2 admite los siguientes perfiles.

| Señal | Tipo crudo | Factor | Offset |
| --- | --- | --- | --- |
| `coolant_temperature` | `u16` | 0.1 | −40 |
| `battery_voltage` | `u8` | 0.05 | 8 |

La conversión de crudo a físico usa una operación multiplicar-sumar fusionada (`std::fma`) con factor y offset binary64 y el redondeo al más cercano predeterminado. El escritor usa el tipo 2 cuando `encodePhysical` obtiene un código sin saturación y `decodeRaw` recupera exactamente los bits de la medición. Guarda el tipo 1 para los demás casos, incluidos los valores no finitos, fuera del rango representable o que perderían precisión al cuantizarse. Así se conserva la medición original junto con la calidad y el resultado emitidos. [Perfiles de escalamiento](escalamiento.md)

### Diagnósticos y códigos

Cada diagnóstico ocupa tres bytes en el orden código, severidad y señal. La señal usa el código del catálogo o `0xFF` si el diagnóstico es global. La causa enclavada usa el mismo bloque con severidad crítica y un ciclo positivo que no puede superar el del registro.

| Campo | Códigos decimales |
| --- | --- |
| Estado | 0 `INIT`, 1 `SELF_TEST`, 2 `OPERATIONAL`, 3 `DEGRADED`, 4 `SAFE_STATE`, 5 `SHUTDOWN` |
| Acción | 0 `INHIBIT_OUTPUT`, 1 `LIMITED_OPERATION`, 2 `NORMAL_OPERATION` |
| Resultado | 0 `VALID`, 1 `INVALID`, 2 `MISSING` |
| Frescura | 0 `NEVER_RECEIVED`, 1 `FRESH`, 2 `RETAINED`, 3 `EXPIRED` |
| Calidad | 0 `IN_RANGE`, 1 `OUT_OF_RANGE`, 2 `NON_FINITE` |
| Severidad | 0 `DEGRADED`, 1 `CRITICAL` |

| Código de diagnóstico | Diagnóstico |
| --- | --- |
| 0 | `SIGNAL_MISSING` |
| 1 | `SIGNAL_EXPIRED` |
| 2 | `SIGNAL_RETAINED` |
| 3 | `SIGNAL_OUT_OF_RANGE` |
| 4 | `SIGNAL_NON_FINITE` |
| 5 | `COOLANT_OVER_TEMPERATURE` |
| 6 | `LOW_OIL_PRESSURE` |
| 7 | `BATTERY_VOLTAGE_OUTSIDE_OPERATING_RANGE` |
| 8 | `POWERTRAIN_INCOHERENCE` |
| 9 | `RECOVERY_PENDING` |

## Lectura y errores

`BinaryReader::read(record)` devuelve `record`, `end` o `error`. La cabecera por sí sola es un archivo válido sin registros. Un flujo vacío carece de cabecera y una cabecera incompleta se considera truncada. Una vez leída la cabecera, el fin de archivo entre registros es limpio y el fin de archivo dentro de un registro se considera truncamiento.

El lector comprueba el rango de longitud antes de leer el resto del registro. Después comprueba el CRC, los campos, la coherencia del registro común y el orden de ciclos. Al primer error se detiene y conserva el último registro entregado. `error()` devuelve la causa y `offset()` indica la posición, desde el byte cero, del inicio de la cabecera o del registro que falló. Los fallos de E/S tienen un código propio.

```cpp
#include "ecu/binary.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char* argv[])
{
    if (argc != 2) {
        return 1;
    }
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) {
        return 1;
    }
    ecu::BinaryReader reader(input);
    ecu::CycleRecord record;
    for (;;) {
        switch (reader.read(record)) {
        case ecu::BinaryReadStatus::record:
            std::cout << record.cycle << '\n';
            break;
        case ecu::BinaryReadStatus::end:
            return 0;
        case ecu::BinaryReadStatus::error:
            std::cerr << "Byte " << reader.offset() << " con " << ecu::binaryErrorMessage(reader.error()) << '\n';
            return 1;
        }
    }
}
```

## Pruebas

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
(cd build/debug && ctest -R '^binary\.' --output-on-failure)
```

Las pruebas contrastan el escritor y el lector con bytes de referencia generados mediante `struct.pack` y `zlib.crc32`. Cubren los extremos de longitud y de ciclo, valores escalados y binary64, truncamiento, corrupción, versión y campos inválidos, orden de ciclos y errores de flujo. Los cinco escenarios se recuperan completos y se comparan registro por registro con el CSV. La prueba de CLI lee el archivo producido por el ejecutable y comprueba la salida del Dashboard.
