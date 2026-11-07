# Eficiencia de las operaciones del simulador

El núcleo utiliza un catálogo pequeño y fijo, instantáneas propias y ciclos lógicos. Las decisiones de estructura de datos buscan conservar esas propiedades y expresar las operaciones mediante algoritmos STL con predicados concretos. El análisis siguiente se refiere al código de Gateway, Control y Scenario; no incluye la conversión de texto ni la escritura en la terminal.

## Tamaños del problema

| Símbolo | Significado | Límite actual |
| --- | --- | --- |
| `n` | Número de señales conocidas | 6 |
| `k` | Recepciones únicas presentes en un lote | Entre 0 y `n` |
| `s` | Tramos de conducción registrados | Hasta 16 |
| `f` | Eventos de falla registrados en la agenda | Hasta 32 |
| `d` | Diagnósticos del resultado de Control | Hasta `n + 1` |

Aunque estos límites están fijados en la implementación, se usan como variables para explicar cómo cambia el costo al ampliar el catálogo o las capacidades. O(1) auxiliar significa que una operación solo necesita un número acotado de contadores, referencias o iteradores adicionales. No significa que el estado completo de señales ocupe memoria constante respecto de `n`.

## Tres operaciones principales

| Operación | Antes | Implementación actual | Tiempo | Memoria auxiliar |
| --- | --- | --- | --- | --- |
| Buscar una señal por ID | Conversión validada de ID a índice | Se conserva el índice directo; no se introduce búsqueda lineal | O(1) | O(1) |
| Validar y publicar un lote | Bucle por índice sobre las ranuras | `for_each` sobre el catálogo, con la misma función `classify` para cada recepción presente | O(n), con `k` clasificaciones | O(1), excluyendo la instantánea de salida |
| Contar señales por resultado | No existía una API de conteo | `count_if` sobre el catálogo, con un predicado que consulta `observe` | O(n) por resultado | O(1) |

El conteo es una capacidad añadida. No se presenta como una aceleración de una función anterior. Los cambios de bucle a algoritmo mantienen el orden de complejidad; su aporte es expresar búsqueda, existencia, recorrido y conteo sin repetir la mecánica de iteración ni las reglas del dominio.

### Búsqueda por identificador

`SignalId` usa identificadores densos: el valor de cada ID coincide con su posición en `signalCatalog`. `detail::signalIndex` convierte el ID y verifica que esté dentro del catálogo antes de acceder a la ranura. `SignalBatch::add`, `SignalSnapshot::observe` y la consulta de duplicados aprovechan esa relación.

No hay recorrido de las señales para localizar un ID. Buscar un ID conocido o rechazar uno desconocido requiere O(1). La observación copia como máximo una muestra, su edad, frescura y resultado, también O(1). Un ID desconocido devuelve `std::nullopt`; un ID conocido sin muestra devuelve una observación `MISSING`.

El `static_assert` del catálogo comprueba la relación ID/posición y los metadatos en compilación. Se conserva su bucle `constexpr`: en C++17 no se debe sustituir por un algoritmo que impida evaluar esa comprobación en compilación. Este índice depende de que el catálogo siga siendo denso; un catálogo con IDs dispersos requeriría revisar esa decisión.

### Validación y publicación

`Gateway::processCycle` rechaza primero un número de ciclo repetido o anterior. Ese rechazo es O(1) y no cambia muestras, diagnósticos ni recepción. Para un ciclo aceptado, `std::for_each` recorre los `n` descriptores y una lambda consulta la ranura correspondiente del lote.

Solo las `k` recepciones presentes ejecutan `classify`; las demás conservan su muestra y su ciclo de recepción. La función de clasificación sigue siendo el único lugar que decide calidad por rango y finitud. El predicado de conteo y el Dashboard no repiten esos límites. Procesar el lote es O(n), incluso si está vacío, porque se consultan todas las ranuras.

La publicación devuelve una instantánea autocontenida por valor. Su copia tiene tamaño O(n), además del recorrido de validación; no se debe describir la publicación completa como una operación O(1). La instantánea de salida ocupa O(n). El recorrido no crea otro arreglo temporal ni asigna memoria dinámica, y usa O(1) auxiliar.

La observación deriva edad, frescura y resultado en el momento de la consulta usando el ciclo y la configuración propios de esa instantánea. No existe una caché de resultados que deba sincronizarse al recibir muestras, omitirlas o cambiar de ciclo.

### Conteo de inválidas y ausentes

`SignalSnapshot::count(status)` aplica `std::count_if` al catálogo. Su lambda consulta `observe(descriptor.id)` y compara el resultado con `status`. Como cada consulta es O(1), un conteo cuesta O(n). El resultado del algoritmo se convierte a `std::size_t`; representa una cantidad no negativa entre cero y `signalCount`.

El recorrido reutiliza la semántica ya publicada por Gateway:

- Nunca recibida o caducada: suma en `MISSING`.
- Vigente y fuera de rango o no finita: suma en `INVALID`.
- Vigente dentro de rango, incluida una muestra retenida dentro de tolerancia: suma en `VALID`.

La suma de las tres categorías es `n`. La antigüedad puede mover una muestra de `INVALID` a `MISSING` sin borrar su calidad original; el conteo observa ese resultado y no necesita una excepción propia. Consultar una instantánea guardada sigue usando su ciclo y su configuración, aunque Gateway avance o se destruya.

El Dashboard consulta `INVALID` y `MISSING` y calcula `VALID = signalCount - INVALID - MISSING`. El resumen requiere dos pasadas: O(2n), que se expresa como O(n), con O(1) auxiliar. Mostrar también las filas añade otro recorrido O(n). Una acumulación de las tres categorías en una sola pasada podría reducir la constante, pero requeriría otra operación o interfaz; con seis señales se mantiene una API de consulta simple sin afirmar una mejora de tiempo medida.

Los contadores describen señales, no severidades ni cantidad de diagnósticos. Una señal `VALID` puede activar un límite operativo crítico de Control. El estado seguro puede permanecer enclavado cuando todas las señales vuelven a ser `VALID`.

## Búsquedas y predicados de Control y Scenario

| Operación | Algoritmo y predicado | Tiempo máximo | Propiedad conservada |
| --- | --- | --- | --- |
| `Diagnostics::primary` | `find_if`: severidad crítica | O(d) | Elige el primer crítico en el orden de diagnóstico; si no existe, el primer hallazgo; vacío devuelve `nullopt`. |
| `Scenario::throttleAt` | `find_if`: fin inclusivo del tramo mayor o igual al ciclo consultado | O(s) | Elige el primer tramo que cubre el ciclo, con los mismos límites contiguos e inclusivos. |
| `Scenario::addFault` | `any_of`: mismo ID e intersección de intervalos inclusivos | O(f) | Rechaza cualquier solapamiento del mismo canal; permite intervalos adyacentes y fallas simultáneas en canales distintos. |

Cada operación necesita O(1) auxiliar y puede terminar al encontrar una coincidencia. Los algoritmos recorren exclusivamente los elementos activos: los `d` diagnósticos, los `s` tramos o los `f` eventos registrados. Las ranuras reservadas pero todavía no utilizadas no participan en las búsquedas.

`find_if` conserva el orden: una búsqueda con otra prioridad podría cambiar la primera causa enclavada aun si detectara los mismos hallazgos. `any_of` expresa que basta una intersección para rechazar una nueva falla; no cuenta todas las coincidencias porque el llamador no necesita esa cantidad.

Las condiciones de salud de Control se derivan de sus diagnósticos existentes. Esa información permite resolver las transiciones sin volver a recorrer las señales para recalcular reglas. El conteo consulta las observaciones directamente y conserva la misma fuente de resultados, sin materializar otra colección.

## Elección de contenedores

| Contenedor | Evaluación para este modelo | Decisión |
| --- | --- | --- |
| `std::array` | Capacidad fija, acceso por índice O(1), elementos contiguos y almacenamiento propio, sin reserva dinámica | Se conserva para catálogo, muestras, lotes, diagnósticos y agendas. |
| `std::vector` | También permite acceso indexado O(1), pero administra tamaño/capacidad y almacenamiento dinámico | Útil para capturar trazas del benchmark; no aporta crecimiento necesario al catálogo fijo. |
| `std::unordered_map` | Búsqueda promedio O(1) y peor caso O(n), con almacenamiento para entradas e índice hash | No mejora el acceso directo por IDs densos; añade estructura y memoria. |
| `std::map` | Búsqueda O(log n), claves ordenadas y almacenamiento de asociaciones | El orden del catálogo ya es explícito y no se necesitan inserciones dinámicas. |
| `std::set` | Búsqueda de claves únicas O(log n); no asocia por sí mismo cada clave con toda la muestra | Las ranuras de `SignalBatch` ya detectan duplicados sin mantener otro conjunto. |
| `std::queue` | Acceso FIFO a los extremos, sin búsqueda arbitraria como parte de su interfaz | No representa la agenda de intervalos: pueden existir fallas simultáneas y consultas por ciclo sin consumir eventos. |

El almacenamiento de muestras y lotes es O(n). Los diagnósticos reservan O(n) y las agendas reservan capacidad para 16 tramos y 32 eventos, aunque solo se recorran los elementos activos. Si esas capacidades se expresan como `Smax` y `Fmax`, el almacenamiento combinado es O(n + Smax + Fmax). Los tamaños exactos en bytes dependen de alineación, representación de `std::optional` y biblioteca estándar; no se deducen únicamente de la complejidad asintótica.

La elección de `array` evita asignaciones dinámicas en el procesamiento de señales, pero no hace gratuitas sus copias. El renderizado de texto y la captura de trazas del benchmark tienen sus propios costos y pueden utilizar memoria dinámica fuera de ese procesamiento.

## Verificación y límites de las conclusiones

Las pruebas deben demostrar conservación de resultados y fronteras, no exigir un tiempo de ejecución determinado. Se comprueban IDs conocidos/desconocidos, valores de rango y no finitos, lotes duplicados, recepción y caducidad, conteos de los tres estados, snapshots independientes, prioridad diagnóstica y búsquedas sobre agendas parcialmente ocupadas. Las pruebas de integración mantienen los escenarios completos y sus estados por ciclo. El resumen se comprueba en ASCII, braille y presentación compacta.

Desde la raíz del proyecto:

```sh
cmake -S . -B build/debug -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/debug --parallel
(cd build/debug && ctest --output-on-failure)
(cd build/debug && ctest -R '^(gateway|control|simulation|dashboard)\.' --output-on-failure)
```

El análisis es de complejidad y estructura, no una medición comparativa de latencia. Usar STL no garantiza por sí solo ejecutar menos instrucciones que un bucle equivalente. Las búsquedas conservan su complejidad y el resumen añade dos recorridos; no se afirma una reducción de tiempos sin medirla con el mismo trabajo y entorno. El benchmark existente mide el núcleo completo y el renderizado, no aísla estas tres operaciones. Sus muestras históricas no demuestran el costo de la implementación actual.
