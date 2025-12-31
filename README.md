Celem naszego projektu jest stworzenie aplikacji typu radio internetowe. Na serwerze TCP, który używałby POSIX sockets, znajdowały by się utwory w formacie WAV. Serwer odtwarzałby je, a klienci mogliby się do niego podłączać i wspólnie słuchać strumieniowanych utworów.
Połączeni z radiem klienci mieliby wgląd do aktualnej kolejki odtwarzania, mogliby dodawać oraz usuwać z niej utwory, przestawiać je w dowolny sposób oraz zlecać przeskoczenie do następnego utworu.
Obsługa aplikacji będzie się odbywać przy pomocy GUI w HTML (REST API). W GUI będzie możliwe śledzenie przebiegu piosenki w czasie rzeczywistym. Operacje te będą synchronizowane wątkami i mutexami.

