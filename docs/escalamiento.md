# Conversión entre valores físicos y crudos

El módulo `ecu_scaling` representa mediciones físicas mediante enteros sin signo. Sus funciones operan sobre valores y perfiles de conversión; no retienen referencias ni modifican muestras, instancias de Gateway o decisiones de Control. Cada operación utiliza tiempo y memoria auxiliar O(1), sin asignación dinámica.

## Relación lineal y perfiles

La decodificación aplica `y = x * F + O`, donde `x` es el entero crudo, `y` el valor físico, `F` el factor y `O` el offset. La inversa calcula `x = (y - O) / F` antes de redondear a entero. `LinearScale` declara `factor`, `offset` y `rawBits`.

| Perfil | ID de señal | Bits sin signo | Crudo | Factor | Offset | Físico representable | Paso físico |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `coolantTemperatureScale` | `coolant_temperature` | 16 | 0…65535 | 0.1 | −40 | −40…6513.5 °C | 0.1 °C |
| `batteryVoltageScale` | `battery_voltage` | 8 | 0…255 | 0.05 | 8 | 8…20.75 V | 0.05 V |

Los extremos provienen de los códigos cero y `2^rawBits - 1`. `representableRange(scale)` devuelve `minimum`, `maximum` y `rawMaximum`. Con un factor negativo se ordenan los extremos físicos: el mínimo físico corresponde entonces al máximo crudo.

`scalingFor(SignalId)` devuelve una copia del perfil de temperatura o batería. Los otros IDs conocidos y los IDs desconocidos devuelven `std::nullopt`, porque no tienen un perfil de conversión definido. Esta ausencia no modifica el catálogo de señales de Gateway.

Ejemplos independientes de los algoritmos de codificación:

| Conversión | Cálculo | Resultado |
| --- | --- | --- |
| Temperatura cruda 1255 | `1255 * 0.1 - 40` | Aproximadamente 85.5 °C |
| Temperatura física 85.5 °C | `(85.5 + 40) / 0.1` | Crudo 1255, sin saturación |
| Batería cruda 116 | `116 * 0.05 + 8` | Aproximadamente 13.8 V |
| Batería cruda 240 | `240 * 0.05 + 8` | Aproximadamente 20 V |
| Batería cruda 255 | `255 * 0.05 + 8` | Aproximadamente 20.75 V |

## Tipos y resultados

La interfaz pública está en `ecu/scaling.hpp`:

| Función | Resultado | Significado |
| --- | --- | --- |
| `scalingFor(id)` | `optional<LinearScale>` | Perfil de una de las dos señales admitidas. |
| `representableRange(scale)` | `optional<RepresentableRange>` | Límites físicos ordenados y máximo crudo. |
| `encodePhysical(value, scale)` | `optional<EncodedValue>` | Código entero y resultado de saturación. |
| `decodeRaw(raw, scale)` | `optional<double>` | Valor físico calculado a partir de un código permitido. |

`EncodedValue::raw` es un `uint16_t` común a ambos perfiles. `rawBits` limita el dominio de cada perfil: usar ese tipo común no amplía el de batería más allá de 255. El argumento de `decodeRaw` es `uint32_t`, para poder rechazar valores que excedan los 16 bits antes de convertirlos a un tipo menor. El llamador debe conservar el valor original hasta consultar el resultado; estrecharlo previamente podría descartar los bits que permiten detectar el error.

`Saturation` tiene tres valores:

- `none`: la entrada física está dentro del intervalo representable, incluidos sus extremos.
- `below_range`: la entrada física es menor que `minimum`; se usa el código que representa ese extremo físico.
- `above_range`: la entrada física es mayor que `maximum`; se usa el código que representa ese extremo físico.

Los nombres describen la posición física, también con factor negativo. Por ejemplo, `LinearScale{-0.5, 10.0, 8}` representa de −117.5 a 10. Una entrada de −118 se satura en el mínimo físico con crudo 255; una entrada de 11 se satura en el máximo físico con crudo cero.

`std::nullopt` indica que la conversión o el perfil se rechazaron. Es distinto de una conversión válida al código cero: este representa −40 °C en temperatura y 8 V en batería. El entero cero no es un marcador de ausencia.

## Redondeo y cuantización

La codificación redondea el valor crudo calculado en `double` mediante `std::round`, sin añadir epsilon. Un empate de esa cantidad se aleja de cero. Como los códigos están acotados a un dominio no negativo, un empate válido selecciona el entero superior.

Este contrato se aplica a la aritmética binaria realizada, no a una interpretación decimal exacta del texto de entrada. Por ejemplo, el cálculo de `(-39.95 + 40) / 0.1` puede dar aproximadamente `0.4999999999999716`: está por debajo de medio paso y produce crudo cero. De modo similar, `(13.825 - 8) / 0.05` puede quedar ligeramente por debajo de `116.5`. No se desplazan estos resultados mediante una tolerancia que también cambiaría entradas realmente inferiores al punto medio.

Los casos de empate se comprueban con valores cuyo crudo calculado sí queda en medio paso. Las pruebas con `nextafter` describen vecinos de esos casos sin suponer que toda resta y división distinguirá dos físicos adyacentes: una operación en coma flotante puede producir el mismo crudo calculado para ambos.

`decodeRaw` usa `std::fma(crudo, factor, offset)`, que realiza la multiplicación y la suma con un único redondeo final. Los resultados siguen siendo valores en coma flotante; un decimal como 13.8 no tiene por qué almacenarse exactamente en binario.

Para los perfiles establecidos y valores físicos no saturados, el error entre entrada y valor reconstruido está acotado por `abs(F) / 2` más el error de coma flotante. Eso corresponde aproximadamente a 0.05 °C en temperatura y 0.025 V en batería. La cota de medio paso no se aplica a entradas saturadas: su distancia al extremo puede ser mayor. Tampoco debe interpretarse como garantía de exactitud decimal de perfiles arbitrarios con una resolución inferior a la precisión de `double`.

La validación de un perfil comprueba sus extremos, pero no garantiza que todos sus códigos contiguos produzcan físicos distintos para cualquier combinación de factor y offset. Un offset muy grande respecto del paso puede hacer que varios códigos se redondeen al mismo `double`. Los perfiles personalizados necesitan verificar esa resolución según su uso. La garantía comprobada mediante recorrido exhaustivo `crudo → físico → crudo` corresponde a los dos perfiles publicados.

## Saturación y seguridad de las conversiones

La codificación comprueba la entrada física frente al intervalo representable antes de redondear. Una entrada situada apenas fuera del intervalo queda marcada como saturada aunque el redondeo del crudo pudiera producir el mismo entero extremo. Los extremos exactos producen ese código con `Saturation::none`.

Después de obtener un crudo calculado finito, se lo acota al dominio entero antes del cast. El tipo de salida nunca recibe una cantidad fuera de su rango representable. Los valores finitos extremos, como el máximo de `double`, se resuelven por comparación con los límites físicos, sin depender de convertir una cantidad enorme directamente a entero.

El módulo rechaza estos casos con `std::nullopt`:

- Factor cero o no finito, u offset no finito.
- Ancho crudo fuera del intervalo de 1 a 16 bits.
- Extremos físicos no finitos o indistinguibles por la precisión de coma flotante.
- Valor físico NaN o infinito, que no posee una codificación entera válida.
- Crudo mayor que el máximo del perfil al decodificar: 256 para batería o 65536 para temperatura, por ejemplo.

La validación del ancho precede al cálculo del máximo crudo. Se permiten factores negativos finitos distintos de cero. Si la aritmética necesaria para codificar no produce una cantidad cruda finita, la conversión se rechaza antes de convertirla a entero.

## Relación con las mediciones de Gateway

El rango representable describe la relación entre el código entero y sus unidades físicas. Gateway conserva su propio rango de aceptación de mediciones: −40…150 °C para temperatura y 9…16 V para batería. Por eso 180 °C y 20 V pueden codificarse sin saturación y seguir siendo muestras `INVALID` en Gateway.

Una conversión saturada tampoco reemplaza el diagnóstico original. Por ejemplo, 30 V se convierte al extremo de batería con `above_range`, pero la medición original permanece en 30 V y conserva su calidad. El módulo recibe una copia del número; no cambia el objeto de muestra, su ciclo de recepción, antigüedad o resultado.

Las señales nunca recibidas no contienen un valor para convertir. Una muestra `MISSING` por caducidad puede conservar su último valor numérico; codificar ese número no la vuelve vigente. Si una muestra contiene NaN o infinito, el llamador conserva el dato y su resultado original y recibe rechazo de la conversión. Ningún fallo de conversión se sustituye silenciosamente por crudo cero.

## Uso con CMake

El módulo se enlaza mediante su propio target y propaga C++17 y el directorio de cabeceras:

```cmake
add_executable(mi_consumidor main.cpp)
target_link_libraries(mi_consumidor PRIVATE ecu_scaling)
```

Ejemplo de uso que comprueba los resultados opcionales:

```cpp
#include "ecu/scaling.hpp"

#include <cmath>

int main()
{
    const auto profile = ecu::scalingFor(ecu::SignalId::coolant_temperature);
    if (!profile) {
        return 1;
    }
    const auto encoded = ecu::encodePhysical(85.5, *profile);
    if (!encoded || encoded->raw != 1255 ||
        encoded->saturation != ecu::Saturation::none) {
        return 1;
    }
    const auto restored = ecu::decodeRaw(encoded->raw, *profile);
    return restored && std::abs(*restored - 85.5) <= 1e-9 ? 0 : 1;
}
```

## Pruebas

Desde la raíz del proyecto:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
(cd build/debug && ctest -R '^scaling\.' --output-on-failure)
```

Las pruebas usan tablas de valores conocidos, fronteras, vecinos de redondeo, parámetros inválidos y saturación. Para los perfiles de temperatura y batería se recorre todo el dominio de códigos: 65 536 y 256 valores, respectivamente, para comprobar `crudo → físico → crudo`. Las comprobaciones con observaciones de Gateway verifican que la conversión no altera las mediciones ni sus resultados.
