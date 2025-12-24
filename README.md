## Opis rozwiązania zadania projektowego.
### Kompilacja programu
Aby uruchomić program należy go skompilować na systemie Linux komendą, a następnie uruchomić(o tym za chwilę)
```
gcc -o main main.c
./main
```
### Wybór trybu zaciągania danych
Zadanie wymaga aby program był w stanie zaciągać dane na 3 sposoby:
1. Z klawiatury
2. Z pliku
3. z pliku /dev/urandom - plik, który dostarcza strumień pseudolosowych bajtów

Wybraną opcję zaciągniemy jako argument przy wywołaniu pliku main. Funkcja **konfiguracja_trybow** obsluguje: sprawdzenie czy podano jeden z założynych argumentów, czy, w przypadku opcji z pliku, podano również plik z danymi(zmienna **sciezka_do_pliku**) oraz przypisanie numeru opcji do zmiennej **tryb_pracy**. W przypadku podania zbyt małej ilości argumentów funkcja wypisuje pomocne menu. Funkcja **strcmp** porównuje dwa ciagi znaków i zwraca 0 jeżeli są takie same.

### Utworzenie procesu P1 i odczytanie danych


   
