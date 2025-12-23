#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> //obsługa nazw zmiennych 
#include <sys/wait.h>
#include <signal.h>

// ------- ZMIENNE GLOBALNE ------
pid_t P1 = 0;
pid_t P2 = 0;
pid_t P3 = 0;

// ------ Zmienne dla trybu pracy dla procesu P1 ------
int tryb_pracy = 0;
char *sciezka_do_pliku = NULL;

// ------ DEKALARACJA FUNKCJI ------
pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa);
int konfiguracja_trybow(int argc, char *argv[]);
void proces_1();
void proces_2();
void proces_3();

int main(int argc, char *argv[]){
    // Obsluga arguemntow przekazywanych do programu
    if (konfiguracja_trybow(argc, argv) != 0){
        // cos innego niz 0 to blad
        return 1;
    }

    // 2. Wypisanie PID P0 - macierzystego
    printf("P0 - PID: %d\n", getpid());

    // 3. Utworzenie P1, P2, P3
    P1 = utworz_proces(proces_1, "P1");
    P2 = utworz_proces(proces_2, "P2");
    P3 = utworz_proces(proces_3, "P3");

    printf("[MAIN] Procesy potomne utworzone: P1=%d, P2=%d, P3=%d\n", P1, P2, P3);

    // 3. Oczekiwanie na zakończenie (sprzątanie) - czekamy na dowolne dziecko, dopóki są jakieś dzieci
    while (wait(NULL) > 0); //Wstymanie pracy P0

    printf("[MAIN] Wszystkie procesy zakończone. Koniec.\n");
    return 0;
}


pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa){
    pid_t pid = fork();

    if(pid == 0){
        funkcja_procesu();
        exit(0); //po zakonczneniu dziecko ginie, aby nie wrocic do maina
    }
    else if(pid < 0){
        perror(nazwa);
        exit(1);
    }

    return pid;
}

int konfiguracja_trybow(int argc, char *argv[]){
    if (argc < 2){
        fprintf(stderr, "Uzycie: %s <tryb>\n", argv[0]);
        fprintf(stderr, "Tryby:\n");
        fprintf(stderr, "\t-i           : Tryb interaktywny (klawiatura)\n");
        fprintf(stderr, "\t-p <plik>    : Odczyt z pliku tekstowego\n");
        fprintf(stderr, "\t-r           : Odczyt z /dev/urandom\n");
        return -1;
    }

}

void proces_1() {
    // Tutaj będzie: Czytanie z pliku/klawiatury -> Konwersja HEX -> Zapis do Shared Memory
    printf(" -> [P1] Uruchomiony. Konwersja HEX i Shared Memory.\n");
    sleep(2); // Symulacja pracy
    printf(" -> [P1] Koniec.\n");
}

void proces_2() {
    // Tutaj będzie: Odczyt z Shared Memory -> Zapis do PIPE
    printf(" -> [P2] Uruchomiony. Most między Shared Memory a Pipe.\n");
    sleep(2);
    printf(" -> [P2] Koniec.\n");
}

void proces_3() {
    // Tutaj będzie: Odczyt z PIPE -> Wypisanie na ekran
    printf(" -> [P3] Uruchomiony. Odczyt z Pipe i ekran.\n");
    sleep(2);
    printf(" -> [P3] Koniec.\n");
}