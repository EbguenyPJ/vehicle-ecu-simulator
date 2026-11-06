# ECU de portafolio

Proyecto en C++17 para simular el flujo de señales de un vehículo. El proyecto incluye la biblioteca `ecu_gateway`, el contrato de seis señales y una instantánea por ciclo. La Gateway recibe muestras, las califica y conserva la última recepción por señal. El núcleo puede probarse con ciclos lógicos, sin depender de un reloj real.

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

Por ejemplo, una muestra recibida en el ciclo 10 queda retenida del 11 al 13 si faltan actualizaciones y caduca en el 14. Una nueva recepción única vuelve a tener edad 0 y obtiene su propia calidad. Control definirá qué operaciones admiten una muestra retenida. Las muestras caducadas se reservan para diagnóstico.

## Propiedad de los datos

La Gateway mantiene un único estado de señales de capacidad fija. Al aceptar un ciclo, publica por valor una copia de ese estado junto con el número de ciclo y el diagnóstico de entrada. Los lotes y las instantáneas usan almacenamiento fijo y el camino de procesamiento no asigna memoria dinámica. Una instantánea que el llamador conserve mantiene sus datos tras procesar el siguiente ciclo.

El coordinador previsto conservará la instantánea del ciclo y pasará la misma instancia por `const SignalSnapshot&` durante cada llamada a Control y Dashboard. Control devolverá su decisión y su diagnóstico, y el Dashboard recibirá ese resultado junto con la instantánea. Así cada consumidor puede leer solo las señales que necesita y la decisión de Control puede asociarse al mismo ciclo.

## Requisitos verificables

| ID | El proyecto debe cumplir | Caso de CTest |
| --- | --- | --- |
| GW-01 | Exponer las seis señales con sus unidades y límites. Aceptar ambos extremos como válidos y distinguir valores finitos fuera de rango y valores no finitos. | `gateway.catalog_and_range` |
| GW-02 | Representar la ausencia inicial, conservar la última muestra y su calidad durante omisiones, caducar al superar la tolerancia y recalificar al recibir una nueva muestra. | `gateway.freshness` |
| GW-03 | Diagnosticar IDs desconocidos y duplicados. Descartar las ocurrencias duplicadas independientemente de su orden, sin renovar su edad ni impedir que se procesen otros canales. | `gateway.malformed_batch` |
| GW-04 | Publicar una instantánea autocontenida que pueda conservarse sin alias al estado interno de la Gateway. | `gateway.snapshot` |
| GW-05 | Procesar un lote por ciclo lógico creciente, calcular la edad a través de saltos y conservar el estado cuando se rechaza un ciclo. | `gateway.cycle_order` |

## Prácticas que se deben evitar

- Representar una señal ausente con cero o convertir la última muestra retenida en una lectura del ciclo actual.
- Tratar una muestra reciente como válida sin consultar su calidad, o considerar vigente una muestra caducada.
- Indexar por un ID sin validarlo. Si en el futuro se decodifican IDs numéricos, validar el valor antes de estrecharlo a `SignalId`.
- Resolver duplicados por orden de llegada o permitir que un duplicado reinicie la edad.
- Reconstruir arreglos paralelos de señales en Control o Dashboard, o guardar referencias a la instantánea más allá de la llamada que la consume.
- Introducir en Gateway reglas de criticidad, transiciones de estado de Control o decisiones de presentación.

## Compilación y pruebas

Ejecuta estos comandos desde la raíz del proyecto en un shell POSIX. Se requiere CMake 3.16 o posterior y un compilador C++17.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build --config Debug
(cd build && ctest -C Debug --output-on-failure)
```

## Alcance

El entregable es una biblioteca de Gateway con sus pruebas. La política operativa y la FSM de Control, el simulador, el coordinador y el Dashboard quedan pendientes de integración. El ciclo es un número lógico. La cadencia física de muestreo y cualquier adaptador de bus requieren contratos propios.

### Responsabilidades previstas

- El **coordinador** será dueño del tick y del orden de ejecución. Entregará cada lote a la Gateway y asociará la instantánea y la decisión de Control al mismo ciclo.
- El **simulador** producirá las lecturas del vehículo. Los guiones de aceptación fijarán el ciclo de cada evento y sus efectos para reproducir el recorrido.
- La **ECU de Control** evaluará la aptitud de las muestras para la operación y emitirá estado, causa y acción simulada según su política. Leerá la instantánea publicada por la Gateway.
- El **Dashboard** presentará la calidad y frescura de las señales y, por separado, el estado y la causa que publique Control.
