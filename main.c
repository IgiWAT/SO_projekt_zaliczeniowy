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

    // ---------------------------------
    pid_t pid_p1;
    pid_t pid_p2;
    pid_t pid_p3;
    // ---------------------------------

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

// ------ ZMIENNE STERUJĄCE ------
volatile int czy_dzialac = 1; // 1 - program działa, 0 = kończymy
volatile int czy_wstrzymany = 0; // 0 = pracuje, 1 = pauza

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
void obsluga_sygnalow(int sygn);
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

    // Zapis PID-ów do pamięci współdzielonej, dzięki temu każdy proces będzie mógł odczytać PIDy rodzenstwa
    dzielona_pamiec->pid_p1 = P1;
    dzielona_pamiec->pid_p2 = P2;
    dzielona_pamiec->pid_p3 = P3;

    #pragma endregion

    #pragma region ZAMYKANIE KONCOWEK w PIPE
    // P0 nie korzysta z potoku, więc musi zamknąć obie końcówki w przeciwnym razie P3 nigdy nie dostanie sygnały EOF
    close(potok[0]);
    close(potok[1]);
    #pragma endregion

    #pragma region SPRZATANIE SYSTEMU
    // Oczekiwanie na zakończenie (sprzątanie) - czekamy na dowolne dziecko, dopóki są jakieś dzieci
    while (wait(NULL) > 0); //Wstymanie pracy P0 - pętla kończy się po wciśnięciu CTRL+D, które dla funkcji read(w P1) zwraca 0(EOF w UNIX), czyli kończy czytanie

    printf("[MAIN] Wszystkie procesy zakończone. Koniec.\n");

    //Sprzatanie po shared_memory
    shmdt(dzielona_pamiec);         // Odlaczenie pamieci od P0
    shmctl(shm_id, IPC_RMID, NULL); // Oznaczamy do usuniecia z systemu
    
    #pragma endregion

    return 0;
}


//Czytanie z pliku/klawiatury/urandom -> Konwersja HEX -> Zapis do Shared Memory
void proces_1() {
    #pragma region Rejestracja sygnałów
    signal(SIGTERM, obsluga_sygnalow); // S1
    signal(SIGUSR1, obsluga_sygnalow); // S2
    signal(SIGUSR2, obsluga_sygnalow); // S3
    #pragma endregion

    #pragma region POCZEKANIE NA UTWORZENIE P2 i P3
    // Czekamy, aż main zapisze PIDy, żebyśmy mogli wysyłać sygnały
    while(dzielona_pamiec->pid_p3 == 0) usleep(1000); 

    printf(" -> [P1] Widzę P2:%d, P3:%d\n", dzielona_pamiec->pid_p2, dzielona_pamiec->pid_p3);
    #pragma endregion

    #pragma region ZMIENNE
    FILE *wejscie = NULL;
    ssize_t odczytane_bajty;
    //unsigned char bufor[1024];
    unsigned char bufor[20]; //na potrzeby testowania pliku i urandom
    int limit_urandom = 0;
    // Bufor HEX: 2 znaki na bajt + 1 na '\0'
    char hex_bufor[(sizeof(bufor) * 2) + 1]; 
    #pragma endregion

    #pragma region WYBOR TRYBU PRACY
    if(tryb_pracy==1){ // Klawiatura
        wejscie = stdin;
        printf(" -> [P1] Tryb Interaktywny. Wpisz tekst i zatwierdź ENTER (CTRL+D konczy)\n");    
    }
    else if(tryb_pracy==2){ // Plik
        wejscie = fopen(sciezka_do_pliku, "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd otwarcia pliku wejściowego");
            exit(1);
        }
        printf(" -> [P1] Otwarto plik: %s\n", sciezka_do_pliku);
    }
    else if(tryb_pracy==3){ // Urandom
        wejscie = fopen("/dev/urandom", "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd otwierania /dev/urandom");
            exit(1);
        }
        printf(" -> [P1] Otwarto plik /dev/urandom\n");   
    }
    #pragma endregion

    printf(" -> [P1] Zaczynam czytać dane...\n");

    // Główna pętla przetwarzania
    // fileno(wejscie) zamienia FILE* na int(deksryptor)
    while (czy_dzialac && (odczytane_bajty = read(fileno(wejscie), bufor, sizeof(bufor))) > 0){
        
        // --- 1. OBSŁUGA PAUZY (Sygnał S2/S3) ---
        while(czy_wstrzymany && czy_dzialac){ 
            sleep(1); // Czekaj aktywnie na wznowienie
        }
        if(!czy_dzialac) break; // Jeśli w trakcie pauzy przyszedł SIGTERM, wychodzimy
        // ---------------------------------------

        #pragma region PRZETWARZANIE DANYCH
        printf(" -> [P1] Pobrano paczkę danych: %lu bajtów.\n", odczytane_bajty);

        // Usuwanie znaku nowej linii (jeśli to nie dane binarne z urandom)
        if (tryb_pracy != 3 && odczytane_bajty > 0 && bufor[odczytane_bajty-1] == '\n'){
            odczytane_bajty--;
        }

        // Jeśli po usunięciu entera bufor jest pusty, pomijamy
        if(odczytane_bajty == 0) continue;

        // Konwersja na HEX
        to_hex((char*)bufor, odczytane_bajty, hex_bufor);
        printf(" -> [P1] Wynik HEX: %s\n", hex_bufor);
        #pragma endregion

        #pragma region ZAPIS DO SHARED MEMORY
        // Czekamy, aż P2 odbierze poprzednie dane (status == 0)
        // WAŻNE: Dodano warunek '&& czy_dzialac', żeby nie zawisnąć przy zamykaniu programu!
        while(dzielona_pamiec->status == 1 && czy_dzialac){
            usleep(1000); 
        }
        if(!czy_dzialac) break; // Wyjście awaryjne

        // Kopiowanie i ustawienie flagi
        strncpy(dzielona_pamiec->dane, hex_bufor, SHARED_SIZE-1);
        dzielona_pamiec->dane[SHARED_SIZE-1]='\0';
        dzielona_pamiec->status = 1;
        #pragma endregion

        sleep(1); //do testowania sygnałów dla pliku i urandom - później usunąć
        
        // Obsługa limitu dla urandom
        if(tryb_pracy==3){ 
            limit_urandom++;
            if(limit_urandom >= 20){
                printf(" -> [P1] Limit testowy urandom osiągnięty.\n");
                break;
            }
            sleep(1);
        }
    }

    #pragma region ZAKONCZENIE
    // Czekamy aż P2 odbierze ostatnią paczkę (żeby nie nadpisać flagi końca)
    while(dzielona_pamiec->status == 1 && czy_dzialac) usleep(1000);

    // Ustawiamy flagę końca tylko jeśli kończymy naturalnie (nie przez SIGTERM)
    if(czy_dzialac) {
        dzielona_pamiec->czy_koniec = 1;
        dzielona_pamiec->status = 1;
    }

    if(tryb_pracy != 1 && wejscie != NULL) fclose(wejscie);
    printf("\n -> [P1] Zakończono pobieranie danych.\n");
    #pragma endregion
}

//Odczyt z Shared Memory -> Zapis do PIPE
void proces_2() {
    #pragma region Rejestracja sygnałów
    signal(SIGTERM, obsluga_sygnalow); // S1
    signal(SIGUSR1, obsluga_sygnalow); // S2
    signal(SIGUSR2, obsluga_sygnalow); // S3
    #pragma endregion

    close(potok[0]); //zamknięcie koncowki do czytania

    printf(" -> [P2] Uruchomiony. Czekam na dane w Shared Memory...\n");    

    #pragma region ODCZYT Z SHARED_MEMORY i zapis do PIPE
    while(czy_dzialac){
        // Obsluga pauzy
        while(czy_wstrzymany && czy_dzialac) sleep(1);
        if (!czy_dzialac) break;

        // Czekamy na dane od P1 (status=1)
        while(dzielona_pamiec->status == 0 && czy_dzialac){
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
    #pragma region Rejestracja sygnałów
    signal(SIGTERM, obsluga_sygnalow); // S1
    signal(SIGUSR1, obsluga_sygnalow); // S2
    signal(SIGUSR2, obsluga_sygnalow); // S3
    #pragma endregion

    close(potok[1]); //zamknięcie koncowki do pisania

    printf(" -> [P3] Uruchomiony. Odczyt z Pipe i ekran.\n");
    
    char bufor_pipe[SHARED_SIZE];
    ssize_t bajty; //signed size_t(system może zwracać -1 przez niektóre funkcje)
    int licznik_jednostek = 0; //licznik elementow w wierszu(max 15)

    printf("\n -> [P3] DANE WYJŚCIOWE:\n");

    // read() blokuje proces dopóki nie pojawią się dane w potoku. Zwraca EOF, gdy P2 zamnie swoją końcówkę do wpisywania potok[1]
    while (czy_dzialac && (bajty = read(potok[0], bufor_pipe, sizeof(bufor_pipe)-1)) > 0){
        // OBSŁUGA PAUZY
        // Uwaga: To zadziała dopiero po odebraniu danych, ale sygnał przerwie read() więc pętla się zakręci
        while (czy_wstrzymany && czy_dzialac) sleep(1);
        if (!czy_dzialac) break;

        bufor_pipe[bajty] = '\0'; //dodajemy znak EOF, bo read tego nie robi

        printf("\n");

        for(int i=0; i<bajty; i+=2){ //iterujemy co dwa, bo 1bajt = 2 znaki HEX
            if (i+1 < bajty){ //sprawdzenie czy mamy parę znaków(zabezpieczenie przed uciętym bajtem)
                printf("%c%c", bufor_pipe[i], bufor_pipe[i+1]);
                licznik_jednostek++;

                if (licznik_jednostek >= 15){
                    printf("\n");
                    licznik_jednostek = 0;
                }
                else{
                    printf(" ");
                }
            }
        }
        if (licznik_jednostek != 0){ //gdyby linia nie była pełna
            printf("\n");
            licznik_jednostek =0;
        }

        fflush(stdout); //wymuszenie wypisania bufora ekranu, żeby tekst nie wisiał w pamięci terminala
    }

    close(potok[0]); //zamknięcie czytania
    printf(" -> [P3] Koniec.\n");
}

// Funkcja obsługi sygnałów z propagacją
void obsluga_sygnalow(int sygn){
    pid_t my_pid = getpid();
    pid_t p1 = dzielona_pamiec->pid_p1;
    pid_t p2 = dzielona_pamiec->pid_p2;
    pid_t p3 = dzielona_pamiec->pid_p3;

    // 1. Obsługa SIGTERM (Koniec)
    if (sygn == SIGTERM){ 
        if(czy_dzialac == 1) { 
            czy_dzialac = 0;
            // Przekaz dalej
            if (my_pid != p1) kill(p1, SIGTERM);
            if (my_pid != p2) kill(p2, SIGTERM);
            if (my_pid != p3) kill(p3, SIGTERM);

            exit(0); //Natychmiastowe wyjście z procesu
        }
    }
    // 2. Obsługa SIGUSR1 (Pauza)
    else if (sygn == SIGUSR1) { 
        if (czy_wstrzymany == 0) { 
            czy_wstrzymany = 1;
            // Przekaz dalej
            if (my_pid != p1) kill(p1, SIGUSR1);
            if (my_pid != p2) kill(p2, SIGUSR1);
            if (my_pid != p3) kill(p3, SIGUSR1);
        }
    } 
    // 3. Obsługa SIGUSR2 (Wznowienie)
    else if (sygn == SIGUSR2) { 
        if (czy_wstrzymany == 1) { 
            czy_wstrzymany = 0;
            // Przekaz dalej
            if (my_pid != p1) kill(p1, SIGUSR2);
            if (my_pid != p2) kill(p2, SIGUSR2);
            if (my_pid != p3) kill(p3, SIGUSR2);
        }
    }
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
