# Vehicle ECU Simulator

Proyecto en C++17 para simular el flujo de señales de un vehículo. La Gateway recibe seis señales, califica sus muestras y publica una instantánea por ciclo. La ECU de Control lee esa instantánea y decide el estado, el diagnóstico y la acción simulada. Las dos bibliotecas se prueban con ciclos lógicos, sin depender de un reloj real.

## Contrato de señales

| ID | Señal | Unidad | Rango inclusivo | Omisiones toleradas |
| --- | --- | --- | --- | --- |
| `speed` | Velocidad | km/h | 0 a 250 | 3 |
| `engine_rpm` | Régimen del motor | rpm | 0 a 8000 | 3 |
| `coolant_temperature` | Temperatura del refrigerante | °C | −40 a 150 | 3 |
| `throttle_position` | Posición del acelerador | % | 0 a 100 | 3 |
| `battery_voltage` | Voltaje de batería | V | 9 a 16 | 3 |
| `oil_pressure` | Presión de aceite | bar | 0 a 10 | 3 |

El catálogo fija la unidad, el rango de aceptación de la muestra y la tolerancia temporal de cada señal. La API representa °C con la cadena ASCII `degC`. Los rangos y plazos son parámetros de la simulación. El catálogo se verifica en compilación para asegurar que los IDs coincidan con sus posiciones, las unidades existan y los límites sean finitos y estén ordenados.

### Recepción y ciclos

1. El productor prepara un `SignalBatch` para un ciclo y añade lecturas con `add(id, value)`. El resultado de la primera inserción de un ID conocido es `inserted`, independientemente de la calidad del valor.
2. Si un ID aparece dos o más veces, el lote descarta todas sus lecturas y devuelve `duplicate_id` desde la segunda ocurrencia. La Gateway conserva la muestra anterior y su ciclo de recepción, si existen. Un ID desconocido devuelve `unknown_id` y se descarta. Los demás canales se procesan normalmente. La instantánea publica un indicador de ID desconocido y marcas de duplicado por señal para el lote aceptado.
3. `Gateway::snapshot()` consulta la última instantánea publicada. Al arrancar devuelve el estado inicial del ciclo 0. `Gateway::processCycle(cycle, batch)` acepta ciclos estrictamente mayores al último, incluso con un lote vacío. Un ciclo repetido o anterior devuelve `std::nullopt` y conserva el estado y los diagnósticos. Si hay un salto, la edad refleja la distancia entre los números de ciclo. Cada muestra del lote aceptado se registra con ese ciclo.
4. `SignalSnapshot::observe(id)` devuelve una observación para cada ID conocido y `std::nullopt` para un ID desconocido. Una observación de arranque tiene frescura `never_received`, sin muestra ni edad.

### Calidad y frescura

Una recepción única registra el valor crudo, su calidad y el ciclo de recepción. Los valores finitos dentro del intervalo, incluidos sus extremos, son `in_range`. Los valores finitos fuera del intervalo son `out_of_range`. NaN y ambos infinitos son `non_finite`. Una muestra inválida puede ser reciente.

| Frescura | Condición |
| --- | --- |
| `never_received` | El ID conocido todavía carece de muestra. |
| `fresh` | La última muestra se recibió en el ciclo actual. Edad 0. |
| `retained` | Hay una muestra anterior con edad de 1 a 3 ciclos en el catálogo inicial. |
| `expired` | La edad supera la tolerancia. La última muestra sigue disponible para diagnóstico con su calidad original. |

Por ejemplo, una muestra recibida en el ciclo 10 queda retenida del 11 al 13 si faltan actualizaciones y caduca en el 14. Una nueva recepción única vuelve a tener edad 0 y obtiene su propia calidad. Control usa la calidad y la frescura para decidir si limita o inhibe la operación.

## Propiedad de los datos

La Gateway mantiene un único estado de señales de capacidad fija. Al aceptar un ciclo, publica por valor una copia de ese estado junto con el número de ciclo y el diagnóstico de entrada. Los lotes y las instantáneas usan almacenamiento fijo y el camino de procesamiento no asigna memoria dinámica. Una instantánea que el llamador conserve mantiene sus datos tras procesar el siguiente ciclo.

`ControlECU::step(const SignalSnapshot&)` lee las observaciones durante la llamada y devuelve un resultado propio por valor. El resultado contiene `cycle`, `state`, `action`, `diagnostics` del ciclo y `latchedFault` si hay una causa enclavada. Control conserva su estado y su diagnóstico de enclavamiento, sin reconstruir el arreglo de señales. El futuro coordinador conservará la instantánea y pasará al Dashboard esa misma instancia junto con el resultado de Control. Los diagnósticos de entrada, como duplicados e IDs desconocidos, siguen disponibles en la instantánea de la Gateway.

## ECU de Control

### Máquina de estados

| Estado | Acción simulada | Política |
| --- | --- | --- |
| `INIT` | Inhibida | El primer ciclo aceptado inicia la ventana de arranque en `t0`. Un ciclo sin hallazgos permite pasar de inmediato a `OPERATIONAL`. Ante un hallazgo degradante, espera durante `t0` y `t0 + 1`. Desde `t0 + 2`, la ausencia de un canal crítico nunca recibido provoca `SAFE_STATE`. Los demás hallazgos degradantes provocan `DEGRADED`. Un hallazgo crítico activa `SAFE_STATE` incluso en el primer ciclo. |
| `OPERATIONAL` | Normal | Una condición degradante limita la operación. Una condición crítica inhibe la operación y enclava `SAFE_STATE`. |
| `DEGRADED` | Limitada | Regresa a `OPERATIONAL` con dos ciclos sanos consecutivos. El primero publica `recovery_pending`. Una nueva falla o un salto en la secuencia interrumpe la confirmación. Una condición crítica enclava `SAFE_STATE`. |
| `SAFE_STATE` | Inhibida | Conserva la causa y el ciclo que originaron el enclavamiento. Sigue publicando hallazgos actuales aunque cambien o desaparezcan. |

Control acepta ciclos positivos estrictamente crecientes. Si la instantánea es del ciclo 0, se repite o retrocede, `step` devuelve `std::nullopt` sin cambiar el estado ni la confirmación de recuperación. `reset()` solo actúa en `SAFE_STATE`. Vuelve a `INIT`, conserva el último ciclo procesado y comienza una nueva ventana con la siguiente instantánea aceptada.

### Política diagnóstica

Los umbrales son parámetros del escenario simulado. La Gateway decide la calidad de cada medición. Control evalúa su aptitud para operar y ejecuta los monitores con sus propias precondiciones. Un umbral operativo se aplica a la muestra fresca y válida de su propia señal. La calidad inválida se sigue diagnosticando mientras la muestra está retenida. Al expirar, el diagnóstico activo de esa señal indica caducidad.

| Monitor | Condición y severidad |
| --- | --- |
| Temperatura y presión de aceite | Crítica si la muestra caducó o su calidad es inválida. Una muestra retenida con calidad válida degrada. Si el canal nunca se recibió, su ausencia se vuelve crítica al terminar la ventana de arranque. |
| Temperatura operativa | Crítica por encima de 110 °C. La medición puede quedar dentro del rango de Gateway y activar este diagnóstico. |
| Presión de aceite operativa | Crítica por debajo de 0,8 bar salvo que una muestra fresca y válida de RPM sea menor de 500 y confirme el motor detenido en el modelo. La falta de RPM aptas no confirma esa condición. |
| Voltaje de batería | Degradante con una muestra fresca y válida fuera del intervalo inclusivo de 11,5 a 15 V, o si falta una muestra fresca y válida. |
| Velocidad, RPM y acelerador | Degradante cuando falta una muestra fresca y válida del canal. |
| Plausibilidad de marcha fija | Crítica si, con velocidad de al menos 20 km/h y acelerador de al menos 20 %, las RPM están fuera de la banda inclusiva de 30 a 60 RPM por km/h. Requiere que las tres muestras estén frescas y sean válidas. |

Control evalúa los monitores independientemente, de modo que una falla de batería no oculta una incoherencia de marcha. Un hallazgo crítico tiene prioridad sobre uno degradante. Para el enclavamiento se elige el primer hallazgo crítico en orden fijo. Primero se revisan calidad, frescura y límites de temperatura y batería en orden de catálogo. Después se evalúan el límite operativo de aceite y la plausibilidad de marcha.

## Requisitos verificables

| ID | El proyecto debe cumplir | Caso de CTest |
| --- | --- | --- |
| GW-01 | Exponer las seis señales con sus unidades y límites. Aceptar ambos extremos como válidos y distinguir valores finitos fuera de rango y valores no finitos. | `gateway.catalog_and_range` |
| GW-02 | Representar la ausencia inicial, conservar la última muestra y su calidad durante omisiones, caducar al superar la tolerancia y recalificar al recibir una nueva muestra. | `gateway.freshness` |
| GW-03 | Diagnosticar IDs desconocidos y duplicados. Descartar las ocurrencias duplicadas independientemente de su orden, sin renovar su edad ni impedir que se procesen otros canales. | `gateway.malformed_batch` |
| GW-04 | Publicar una instantánea autocontenida que pueda conservarse sin alias al estado interno de la Gateway. | `gateway.snapshot` |
| GW-05 | Procesar un lote por ciclo lógico creciente, calcular la edad a través de saltos y conservar el estado cuando se rechaza un ciclo. | `gateway.cycle_order` |
| CTRL-01 | Inhibir durante el arranque cuando faltan datos, operar al recibir un ciclo sano y detectar una falla crítica desde el primer ciclo. | `control.startup` |
| CTRL-02 | Limitar la operación ante un hallazgo degradante y exigir dos ciclos sanos consecutivos para recuperarse. | `control.degradation` |
| CTRL-03 | Evaluar los límites de operación y distinguir la falta de una muestra de su caducidad. | `control.operating_limits` |
| CTRL-04 | Evaluar fallas simultáneas y resolver el enclavamiento con prioridad y desempate deterministas. | `control.concurrent_faults` |
| CTRL-05 | Retener la causa y el ciclo originales al inhibir en `SAFE_STATE`, y volver a `INIT` con un reinicio explícito. | `control.safe_latch` |
| CTRL-06 | Rechazar ciclos inválidos sin modificar el estado ni contar para la recuperación. | `control.cycle_order` |

## Prácticas que se deben evitar

- Representar una señal ausente con cero o convertir la última muestra retenida en una lectura del ciclo actual.
- Tratar una muestra reciente como válida sin consultar su calidad, o considerar vigente una muestra caducada.
- Indexar por un ID sin validarlo. Si en el futuro se decodifican IDs numéricos, validar el valor antes de estrecharlo a `SignalId`.
- Resolver duplicados por orden de llegada o permitir que un duplicado reinicie la edad.
- Reconstruir arreglos paralelos de señales en Control o Dashboard, o guardar referencias a la instantánea más allá de la llamada que la consume.
- Introducir en Gateway reglas de criticidad, transiciones de estado de Control o decisiones de presentación.
- Omitir un monitor independiente porque ya existe otro hallazgo o interpretar unas RPM ausentes como prueba de que el motor está detenido.
- Reemplazar la causa enclavada por un diagnóstico posterior o liberar `SAFE_STATE` por la sola desaparición de una falla.

## Compilación y pruebas

Ejecuta estos comandos desde la raíz del proyecto en un shell POSIX. Se requiere CMake 3.16 o posterior y un compilador C++17.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug
(cd build && ctest -C Debug --output-on-failure)
```

## Alcance actual

El entregable incluye las bibliotecas de Gateway y Control y sus pruebas en CTest. El ciclo es un número lógico y las acciones de Control son simuladas. El simulador, el coordinador y el Dashboard quedan pendientes de integración. La cadencia física de muestreo y cualquier adaptador de bus requieren contratos propios.

### Responsabilidades previstas

- El **coordinador** será dueño del tick y del orden de ejecución. Entregará cada lote a la Gateway y asociará la instantánea y la decisión de Control al mismo ciclo.
- El **simulador** producirá las lecturas del vehículo. Los guiones de aceptación fijarán el ciclo de cada evento y sus efectos para reproducir el recorrido.
- El **Dashboard** presentará la calidad y frescura de las señales y, por separado, el estado y la causa que publique Control.
