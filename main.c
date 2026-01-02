#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> //obsługa nazw zmiennych 
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h> //shared_memory
#include <sys/shm.h> //shared_memory

#pragma region ZMIENNE GLOBALNE dla SHARED_MEMORY

#define SHARED_SIZE 4096 //rozmiar pamięci współdzielonej(musi pomieścić dane hex)

//struktura, która będzie się znajdować w pamięci wspólnej
typedef struct{
    volatile int status;     // 0=Puste (P1 moze pisac), 1=Pelne (P2 moze czytac)
    volatile int czy_koniec; // 0=dzialanie, 1=koniec
    char dane[SHARED_SIZE];  //bufor na dane HEX
} DaneWspolne;

// Globalny wskaznik, żeby procesy go widziały
DaneWspolne *dzielona_pamiec = NULL;
int shm_id = 0;
#pragma endregion

#pragma region ZMIENNE GLOBALNE
pid_t P1 = 0;
pid_t P2 = 0;
pid_t P3 = 0;

// ------ Zmienne dla procesów ------
// Dla P1:
int tryb_pracy = 0;
char *sciezka_do_pliku = NULL;

//Dla komunikacji PIPE:
int potok[2]; 
// potok[0] -> wyjscie (czytanie)
// potok[1] -> wejście (pisanie)

#pragma endregion

#pragma region DEKALARACJE FUNKCJI

pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa);
int konfiguracja_trybow(int argc, char *argv[]);
void to_hex(const char *ascii, size_t len, char *hex);
void proces_1();
void proces_2();
void proces_3();

#pragma endregion

int main(int argc, char *argv[]){
    // Obsluga arguemntow przekazywanych do programu
    if (konfiguracja_trybow(argc, argv) != 0){
        // cos innego niz 0 to blad
        return 1;
        //fprintf(stdout, "Tryb pracy: %d\n", tryb_pracy);
    }

    printf("P0 - PID: %d\n", getpid()); //P0 - proces macierzysty

    #pragma region TWORZENIE PAMIECI WSPOLDZIELONEJ

    shm_id = shmget(IPC_PRIVATE, sizeof(DaneWspolne), 0666 | IPC_CREAT); //IPC_PRIVATE - server(P0)-client(P1/P2)
    if(shm_id < 0){
        perror("Blad shmget");
        return 1;
    }

    //Podlaczenie pamieci do procesu P0
    dzielona_pamiec = (DaneWspolne*)shmat(shm_id, NULL, 0);
    if (dzielona_pamiec == (DaneWspolne*)-1){
        perror("Blad shmat");
        return 1;
    }

    //inicjalizacja pamieci
    dzielona_pamiec->status = 0; //gotowy na zapis
    dzielona_pamiec->czy_koniec = 0; //nieskonczone
    memset(dzielona_pamiec->dane, 0, SHARED_SIZE);

    printf("[MAIN] Pamięć współdzielona utworzona (ID: %d)\n", shm_id);

    #pragma endregion

    #pragma region TWORZENIE POTOKU PIPE
    if (pipe(potok) == -1){
        perror("[MAIN] Błąd tworzenia potoku");
        return 1;
    }
    printf("[MAIN] Potok utworzony.\n");

    #pragma endregion

    #pragma region TWORZENIE PROCESOW P1,P2,P3
    P1 = utworz_proces(proces_1, "P1");
    P2 = utworz_proces(proces_2, "P2");
    P3 = utworz_proces(proces_3, "P3");

    printf("[MAIN] Procesy potomne utworzone: P1=%d, P2=%d, P3=%d\n", P1, P2, P3);

    #pragma endregion

    #pragma region ZAMYKANIE KONCOWEK w PIPE
    // P0 nie korzysta z potoku, więc musi zamknąć obie końcówki w przeciwnym razie P3 nigdy nie dostanie sygnały EOF
    close(potok[0]);
    close(potok[1]);
    #pragma endregion

    #pragma region SPRZATANIE SYSTEMU
    // Oczekiwanie na zakończenie (sprzątanie) - czekamy na dowolne dziecko, dopóki są jakieś dzieci
    while (wait(NULL) > 0); //Wstymanie pracy P0

    printf("[MAIN] Wszystkie procesy zakończone. Koniec.\n");

    //Sprzatanie po shared_memory
    shmdt(dzielona_pamiec);         // Odlaczenie pamieci od P0
    shmctl(shm_id, IPC_RMID, NULL); // Oznaczamy do usuniecia z systemu
    
    #pragma endregion

    return 0;
}


//Czytanie z pliku/klawiatury/urandom -> Konwersja HEX -> Zapis do Shared Memory
void proces_1() {
    #pragma region ZMIENNE
    FILE *wejscie = NULL;
    size_t odczytane_bajty; //typ liczbowy bez znaku
    unsigned char bufor[1024];
    int limit_urandom = 0; //Zabezpiecznie przy czytaniu urandom
    char hex_bufor[(sizeof(bufor) * 2) + 1]; //każdy bajt to 2 znaki HEX + 1 bajt na znak konca napisu \0
    #pragma endregion

    #pragma region WYBOR TRYBU PRACY
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
    #pragma endregion

    printf(" -> [P1] Zaczynam czytać dane...\n");

    // fread - wczytuje określoną ilość danych ze strumienia
    while ((odczytane_bajty = fread(bufor, 1, sizeof(bufor), wejscie)) > 0){
        #pragma region POBRANIE DANYCH -> ZAMIANA NA HEX
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
        #pragma endregion

        if(tryb_pracy==3){ 
            limit_urandom++;
            if(limit_urandom >= 5){
                printf(" -> [P1] Limit testowy urandom osiągnięty.\n");
                break;
            }
            sleep(1);
        }

        #pragma region ZAPIS do SHARED MEMORY

        // Czekamy aż P2 odbierze dane (status=0)
        while(dzielona_pamiec->status == 1){
            usleep(1000); //1ms
        }

        // Kopiujemy dane do pamięci współdzielonej
        strncpy(dzielona_pamiec->dane, hex_bufor, SHARED_SIZE-1); //kopiuje dane aż nie napotka znaku konca linii \0 strncpy(cel, zrodlo, max_znakow)
        dzielona_pamiec->dane[SHARED_SIZE-1]='\0';

        //informujemy P2, że dane są gotowe
        dzielona_pamiec->status = 1;
        #pragma endregion
    }

    #pragma region KONIEC ZAPISYWANIA DO SHARED_MEMORY
    //czekamy aż P2 odbierze ostanią daną
    while(dzielona_pamiec->status == 1) usleep(1000);

    dzielona_pamiec->czy_koniec=1; //flaga konca
    dzielona_pamiec->status=1;

    #pragma endregion

    if(tryb_pracy != 1 && wejscie != NULL) fclose(wejscie);
    printf(" -> [P1] Zakończono pobieranie danych.\n");
}

//Odczyt z Shared Memory -> Zapis do PIPE
void proces_2() {
    close(potok[0]); //zamknięcie koncowki do czytania

    printf(" -> [P2] Uruchomiony. Czekam na dane w Shared Memory...\n");    

    #pragma region ODCZYT Z SHARED_MEMORY i zapis do PIPE
    while(1){
        // Czekamy na dane od P1 (status=1)
        while(dzielona_pamiec->status == 0){
            // Jeśli P1 jeszcze nic nie dał, sprawdzamy czy może już skończył? Ale P1 ustawia status=1 również przy wysyłaniu sygnału końca, więc główne sprawdzenie końca jest poniżej.
            usleep(1000);
        }

        if(dzielona_pamiec->czy_koniec == 1){
            printf(" -> [P2] Odebrano sygnał końca pracy. Zamykam potok\n");
            break;
        }

        //printf(" -> [P2] POBRANO z SHM: %s\n", dzielona_pamiec->dane);

        #pragma region ZAPIS DO PIPE
        int len = strlen(dzielona_pamiec->dane);
        if (len>0){
            // write(deskryptor, bufor, ilosc_bajtow)
            if (write(potok[1], dzielona_pamiec->dane, len) == -1){
                perror(" -> [P2] Błąd zapisu do potoku");
                break;
            }
            printf(" -> [P2] Przekazano do PIPE %d bajtów.\n", len);
        }
        #pragma endregion

        //czyszczenie bufora dla bezpieczenstwa
        memset(dzielona_pamiec->dane, 0, SHARED_SIZE); //wypełnia pamięć bajtem

        // Informujemy P1, że odebraliśmy dane
        dzielona_pamiec->status = 0;
    }
    #pragma endregion

    close(potok[1]); // zamknięcie potoku do wpisywania
    printf(" -> [P2] Koniec.\n");
}

//Odczyt z PIPE -> Wypisanie na ekran
void proces_3() {
    close(potok[1]); //zamknięcie koncowki do pisania

    printf(" -> [P3] Uruchomiony. Odczyt z Pipe i ekran.\n");
    
    char bufor_pipe[SHARED_SIZE];
    ssize_t bajty;

    // read() blokuje proces dopóki nie pojawią się dane w potoku. Zwraca EOF, gdy P2 zamnie swoją końcówkę do wpisywania potok[1]
    while ((bajty = read(potok[0], bufor_pipe, sizeof(bufor_pipe)-1)) > 0){
        bufor_pipe[bajty] = '\0'; //dodajemy znak EOF, bo read tego nie robi
        printf("\n*** [P3] OTRZYMANO DANE HEX: ***\n%s\n********************************\n", bufor_pipe);
    }

    close(potok[0]); //zamknięcie czytania
    printf(" -> [P3] Koniec.\n");
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
        fprintf(stderr, "\t-f <plik>    : Odczyt z pliku tekstowego\n");
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
