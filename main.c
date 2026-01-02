#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> //obsługa nazw zmiennych 
#include <sys/wait.h>
#include <signal.h>
#include <string.h>

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
    //fprintf(stdout, "Tryb pracy: %d\n", tryb_pracy);


    // 2. Wypisanie PID P0 - macierzystego
    printf("P0 - PID: %d\n", getpid());

    // 3. Utworzenie P1, P2, P3
    P1 = utworz_proces(proces_1, "P1");
    //P2 = utworz_proces(proces_2, "P2");
    //P3 = utworz_proces(proces_3, "P3");

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

    if (strcmp(argv[1], "-i") == 0){
        tryb_pracy = 1; //Z klawiatury
    }
    else if(strcmp(argv[1], "-f") == 0){
        if (argc < 3){
            fprintf(stderr, "Nie podano nazwy pliku\n");
            return -1;
        }
        tryb_pracy = 2; // z pliku
        sciezka_do_pliku = argv[2];
    }
    else if(strcmp(argv[1], "-r") == 0){
        tryb_pracy = 3; // /dev/urandom
    }
    else{
        fprintf(stderr, "Nieznany tryb: %s\n", argv[1]);
        return -1; //Blad
    }

    return 0; //zakonczono poprawnie

}

void to_hex(const char *ascii, size_t len, char *hex){
    const char hex_map[] = "0123456789ABCDEF";
    
    for (size_t i=0; i<len; i++){
        unsigned char bajt = (unsigned char)ascii[i];
        // Pierwszy znak HEX - 4bity
        hex[i*2] = hex_map[bajt >> 4];
        // Nastepne 4 bity
        hex[i*2 + 1] = hex_map[bajt & 0x0F];
    }
    hex[len*2] = '\0'; //End of line
}

void proces_1() {
    // Tutaj będzie: Czytanie z pliku/klawiatury -> Konwersja HEX -> Zapis do Shared Memory
    FILE *wejscie = NULL;
    size_t odczytane_bajty; //typ liczbowy bez znaku
    unsigned char bufor[1024];
    int limit_urandom = 0; //Zabezpiecznie przy czytaniu urandom
    char hex_bufor[(sizeof(bufor) * 2) + 1]; //każdy bajt to 2 znaki HEX + 1 bajt na znak konca napisu \0


    if(tryb_pracy==1){ //Klawiatura
        wejscie = stdin;
        printf(" -> [P1] Tryb Interaktywny. Wpisz tekst i zatwierdź ENTER(CTRL+D zakonczy)\n");    
    }
    else if(tryb_pracy==2){ //Plik
        wejscie = fopen(sciezka_do_pliku, "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd otwarcia pliku wejściowego");
            exit(1);
        }
        printf(" -> [P1] Otwarto plik: %s\n", sciezka_do_pliku);
    }
    else if(tryb_pracy==3){
        wejscie = fopen("/dev/urandom", "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd otwierania/dev/urandom");
            exit(1);
        }
        printf(" -> [P1] Otwarto plik dev/urandom\n");   
    }

    printf(" -> [P1] Zaczynam czytać dane...\n");

    // fread - wczytuje określoną ilość danych ze strumienia
    while ((odczytane_bajty = fread(bufor, 1, sizeof(bufor), wejscie)) > 0){
        printf(" -> [P1] Pobrano paczkę danych: %lu bajtów.\n", odczytane_bajty);

        // !!! - należ uważać, że w odcztane_bajty wlicza się znak nowej linii (0x0A), dlatego należy to usunąć w funkcji
        if (tryb_pracy != 3 && odczytane_bajty > 0 && bufor[odczytane_bajty-1] == '\n'){
            odczytane_bajty--; //bo znak nowej linii jest ostatnim bajtem
        }

        //jesli po usunieciu entera nic nie zostalo (np. wcisnieto sam ENTER) pomijamy pętlę
        if(odczytane_bajty == 0){
            continue;
        }

        to_hex((char*)bufor, odczytane_bajty, hex_bufor);

        printf(" -> [P1] Wynik HEX: %s\n", hex_bufor);

        if(tryb_pracy==3){
            limit_urandom++;
            if(limit_urandom >= 5){
                printf(" -> [P1] Limit testowy urandom osiągnięty.\n");
                break;
            }
            sleep(1);
        }
    }

    if(tryb_pracy != 1 && wejscie != NULL){
        fclose(wejscie);
    }
    printf(" -> [P1] Zakończono pobieranie danych.\n");
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