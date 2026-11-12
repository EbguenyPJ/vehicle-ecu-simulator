# Registro por ciclo y CSV

La opción `--csv` guarda una fila por ciclo junto con la salida habitual del Dashboard. La ruta se abre después de validar los argumentos y preparar el escenario. Si el archivo existe, la escritura lo reemplaza.

```sh
./build/vehicle-ecu-simulator faults --dashboard ascii --csv "recorrido con fallas.csv"
```

El escenario `faults` produce 42 registros. Estos ciclos permiten comprobar la caducidad, la recuperación de la temperatura y la conservación de la causa enclavada durante el apagado.

| Ciclo | Estado de Control | Resultado de temperatura | Ciclo de la causa enclavada |
| --- | --- | --- | --- |
| 1 | `SELF_TEST` | `VALID` | Vacío |
| 17 | `SAFE_STATE` | `MISSING` | 17 |
| 18 | `SAFE_STATE` | `VALID` | 17 |
| 20 | `SAFE_STATE` | `VALID` | 17 |
| 42 | `SHUTDOWN` | `VALID` | 17 |

En el ciclo 20, la medición de 130 °C es `VALID` para Gateway y aparece junto al diagnóstico crítico de sobretemperatura de Control.

## Registro común y observador

`CycleRecord`, en `ecu/record.hpp`, posee sus datos. `makeRecord(snapshot, result)` copia las seis observaciones en el orden de `signalCatalog`, la configuración de antigüedad, los indicadores de duplicados e IDs desconocidos, el estado, la acción, los diagnósticos actuales y la causa enclavada. La función devuelve `std::nullopt` si los ciclos difieren o los datos son incoherentes. Se puede conservar un registro después de que termine la llamada al observador.

`recordObserver(destino)` entrega el registro a `destino.write(const CycleRecord&)`. El destino devuelve un `bool`. Si falla la captura o la escritura, el observador devuelve `false` y `runScenario` termina en ese ciclo. El resultado de Control de ese ciclo ya fue emitido. Los observadores que devuelven `void` siguen disponibles para consumidores que solo reciben los datos.

El ejecutable abre y cierra el archivo y comunica errores de apertura, escritura, vaciado o cierre con un código de salida distinto de cero. `CsvWriter` recibe un `std::ostream&`. `write(record)` devuelve `true` al entregar la fila al flujo. `finish()` escribe la cabecera si el flujo todavía carece de registros y vacía el búfer. El llamador conserva la propiedad del flujo y debe comprobar el cierre cuando usa un archivo.

```cpp
ecu::CsvWriter writer(output);
const bool completed = ecu::runScenario(scenario, ecu::recordObserver(writer));
const bool flushed = writer.finish();
// El llamador comprueba completed y flushed, y cierra output si es un archivo.
```

El módulo `ecu_record` expone el registro y el adaptador. Los consumidores CMake que leen o escriben CSV enlazan `ecu_csv`, que incluye la dependencia de `ecu_record`.

## Formato CSV versión 1

La primera línea contiene los nombres de las 74 columnas. Cada fila usa comas, punto decimal, nombres fijos y campos vacíos donde corresponde. El escritor emite LF. El lector acepta LF, CRLF y una última fila completa sin salto de línea. Espera campos sin comillas y hasta 4096 bytes por línea, excluyendo su terminador.

La tabla sigue el orden de las columnas.

| Grupo | Columnas | Representación |
| --- | --- | --- |
| Ciclo y Control | `version,cycle,max_age_cycles,control_state,control_action,unknown_id,diagnostic_count` | `version` contiene `1`. Los ciclos y el límite de edad son enteros decimales sin signo de 64 bits. Los indicadores usan `0` o `1`. El conteo de diagnósticos va de 0 a 7. |
| Señales | Para cada señal, `value,quality,received_at,age,freshness,status,duplicate` precedidos por su ID y `_` | `value` usa la unidad física del catálogo. `received_at` y `age` son ciclos. `duplicate` corresponde al lote del ciclo. |
| Diagnósticos actuales | `diag_0_code,diag_0_severity,diag_0_signal` hasta `diag_6_code,diag_6_severity,diag_6_signal` | Se guardan en el orden emitido por Control. `diagnostic_count` indica los grupos usados. Las columnas de grupos restantes quedan vacías. |
| Causa enclavada | `latched_cycle,latched_code,latched_severity,latched_signal` | El ciclo y el diagnóstico conservan la primera causa crítica. Las cuatro columnas quedan vacías cuando falta una causa. |

Los prefijos de señales son `speed`, `engine_rpm`, `coolant_temperature`, `throttle_position`, `battery_voltage` y `oil_pressure`, en ese orden. Los campos de señal de un diagnóstico usan estos IDs. Un diagnóstico sin señal específica lleva ese campo vacío.

| Campo | Valores de texto |
| --- | --- |
| `control_state` | `INIT`, `SELF_TEST`, `OPERATIONAL`, `DEGRADED`, `SAFE_STATE`, `SHUTDOWN` |
| `control_action` | `INHIBIT_OUTPUT`, `LIMITED_OPERATION`, `NORMAL_OPERATION` |
| `quality` | `IN_RANGE`, `OUT_OF_RANGE`, `NON_FINITE` |
| `freshness` | `NEVER_RECEIVED`, `FRESH`, `RETAINED`, `EXPIRED` |
| `status` | `VALID`, `INVALID`, `MISSING` |
| `severity` | `DEGRADED`, `CRITICAL` |
| `code` | `SIGNAL_MISSING`, `SIGNAL_EXPIRED`, `SIGNAL_RETAINED`, `SIGNAL_OUT_OF_RANGE`, `SIGNAL_NON_FINITE`, `COOLANT_OVER_TEMPERATURE`, `LOW_OIL_PRESSURE`, `BATTERY_VOLTAGE_OUTSIDE_OPERATING_RANGE`, `POWERTRAIN_INCOHERENCE`, `RECOVERY_PENDING` |

Una señal nunca recibida tiene `value`, `quality`, `received_at` y `age` vacíos, con `NEVER_RECEIVED` y `MISSING`. Una señal caducada conserva esas cuatro columnas y lleva `EXPIRED` y `MISSING`. Los valores finitos se escriben con `max_digits10` para recuperar el mismo `double`, incluido el signo de cero. `nan`, `+inf` y `-inf` identifican las categorías no finitas. Los NaN leídos se representan como NaN silenciosos.

La lectura y la escritura requieren ciclos estrictamente crecientes. Un registro inicial puede usar el ciclo 0 y el ejecutable empieza en 1. El lector comprueba la cabecera, la versión, el ancho de fila, los valores permitidos y la coherencia entre recepción, edad, frescura y resultado. Las mediciones y decisiones se entregan tal como se publicaron. Una edición que conserve la estructura y la coherencia puede leerse como otro registro válido.

## Lectura desde C++

`CsvReader::read(record)` devuelve `record`, `end` o `error`. Solo `record` reemplaza el objeto de salida. Tras un error, `line()` indica la línea y `error()` conserva la causa. Las filas que el lector ya entregó siguen disponibles para el llamador. Un archivo con solo la cabecera produce fin limpio. Un archivo vacío se rechaza por cabecera ausente.

```cpp
#include "ecu/csv.hpp"

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
    ecu::CsvReader reader(input);
    ecu::CycleRecord record;
    for (;;) {
        switch (reader.read(record)) {
        case ecu::CsvReadStatus::record:
            std::cout << record.cycle << '\n';
            break;
        case ecu::CsvReadStatus::end:
            return 0;
        case ecu::CsvReadStatus::error:
            std::cerr << "Linea " << reader.line() << " con " << ecu::csvErrorMessage(reader.error()) << '\n';
            return 1;
        }
    }
}
```

## Pruebas

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
(cd build/debug && ctest -R '^csv\.' --output-on-failure)
```

Las pruebas cubren lectura y escritura con configuraciones regionales distintas, extremos de `double` y `uint64_t`, registros incompletos y errores de flujo. Un destino en memoria conserva los registros recibidos y falla en un ciclo elegido para comprobar la propagación del error. Los cinco escenarios se escriben y leen completos. La prueba de CLI abre un archivo real, verifica sus 42 registros y compara la salida del Dashboard con la ejecución habitual.
