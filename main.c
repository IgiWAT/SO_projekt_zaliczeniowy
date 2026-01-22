#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 

#pragma region ZMIENNE GLOBALNE dla SHARED_MEMORY

#define SHARED_SIZE 4096 // Rozmiar pamieci wspoldzielonej

// Struktura w pamieci wspolnej
typedef struct{
    volatile int status;     // 0=Puste (P1 moze pisac), 1=Pelne (P2 moze czytac)
    volatile int czy_koniec; // 0=dzialanie, 1=koniec

    // PIDy do komunikacji sygnałowej
    pid_t pid_p1;
    pid_t pid_p2;
    pid_t pid_p3;

    int wielkosc_danych;
    char dane[SHARED_SIZE]; 
} DaneWspolne;

DaneWspolne *dzielona_pamiec = NULL;
int shm_id = 0;
#pragma endregion

#pragma region ZMIENNE GLOBALNE
pid_t P1 = 0;
pid_t P2 = 0;
pid_t P3 = 0;

volatile int czy_dzialac = 1; 
volatile int czy_wstrzymany = 0; 
volatile int tryb_hex = 1; 

// Zmienne konfiguracyjne
int tryb_pracy = 0;
char *sciezka_do_pliku = NULL;

int potok[2]; 
#pragma endregion

#pragma region DEKLARACJE FUNKCJI
pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa);
int konfiguracja_trybow(int argc, char *argv[]);
void to_hex(const char *ascii, size_t len, char *hex);
void obsluga_sygnalow(int sygn);
void proces_1();
void proces_2();
void proces_3();
#pragma endregion

int main(int argc, char *argv[]){
    // Konfiguracja argumentów
    if (konfiguracja_trybow(argc, argv) != 0){
        return 1;
    }

    // P0 - proces macierzysty
    // printf("P0 - PID: %d\n", getpid()); 

    #pragma region TWORZENIE PAMIECI WSPOLDZIELONEJ
    shm_id = shmget(IPC_PRIVATE, sizeof(DaneWspolne), 0666 | IPC_CREAT);
    if(shm_id < 0){
        perror("Blad shmget");
        return 1;
    }

    dzielona_pamiec = (DaneWspolne*)shmat(shm_id, NULL, 0);
    if (dzielona_pamiec == (DaneWspolne*)-1){
        perror("Blad shmat");
        return 1;
    }

    // Inicjalizacja pamięci
    dzielona_pamiec->status = 0; 
    dzielona_pamiec->czy_koniec = 0;
    memset(dzielona_pamiec->dane, 0, SHARED_SIZE);
    #pragma endregion

    #pragma region TWORZENIE POTOKU
    if (pipe(potok) == -1){
        perror("[MAIN] Błąd tworzenia potoku");
        return 1;
    }
    #pragma endregion

    #pragma region TWORZENIE PROCESOW
    P1 = utworz_proces(proces_1, "P1");
    P2 = utworz_proces(proces_2, "P2");
    P3 = utworz_proces(proces_3, "P3");

    printf("[MAIN] System uruchomiony. PIDy: P1=%d, P2=%d, P3=%d\n", P1, P2, P3);
    
    // Zapis PIDów do pliku (dla Managera)
    FILE *plik_pid = fopen("pidy_procesow.txt", "w");
    if (plik_pid) {
        fprintf(plik_pid, "%d %d %d", P1, P2, P3);
        fclose(plik_pid);
    } else {
        perror("[MAIN] Nie udało się zapisać pliku z PIDami");
    }

    // Zapis PID-ów do pamięci współdzielonej (dla procesów potomnych)
    dzielona_pamiec->pid_p1 = P1;
    dzielona_pamiec->pid_p2 = P2;
    dzielona_pamiec->pid_p3 = P3;
    #pragma endregion

    #pragma region ZAMYKANIE KONCOWEK w PIPE (dla P0)
    close(potok[0]);
    close(potok[1]);
    #pragma endregion

    #pragma region OCZEKIWANIE I SPRZATANIE
    // P0 czeka na zakończenie wszystkich dzieci
    while (wait(NULL) > 0); 

    printf("[MAIN] Wszystkie procesy zakończone. Sprzątanie...\n");

    unlink("pidy_procesow.txt"); 

    shmdt(dzielona_pamiec);         
    shmctl(shm_id, IPC_RMID, NULL); 
    
    #pragma endregion

    return 0;
}

// P1: Czytanie -> Konwersja -> Shared Memory
void proces_1() {
    signal(SIGTERM, obsluga_sygnalow);
    signal(SIGUSR1, obsluga_sygnalow); 
    signal(SIGUSR2, obsluga_sygnalow); 
    signal(SIGALRM, obsluga_sygnalow); 

    // Czekamy na inicjalizację PIDów w pamięci
    while(dzielona_pamiec->pid_p3 == 0) usleep(1000); 

    FILE *wejscie = NULL;
    ssize_t odczytane_bajty;
    
    // --- BUFOR PRODUKCYJNY ---
    // Zwiększamy bufor wejściowy. Musi być mniejszy niż (SHARED_SIZE / 2),
    // ponieważ w trybie HEX 1 bajt wejścia = 2 bajty wyjścia.
    // SHARED_SIZE = 4096, więc bezpiecznie max ~2000. Dajemy 1024.
    unsigned char bufor[1024]; 
    char hex_bufor[(sizeof(bufor) * 2) + 1]; 

    if(tryb_pracy==1){ // Klawiatura
        wejscie = stdin;
        printf(" -> [P1] Tryb Interaktywny. Wpisuj tekst...\n");    
    }
    else if(tryb_pracy==2){ // Plik
        wejscie = fopen(sciezka_do_pliku, "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd pliku");
            exit(1);
        }
    }
    else if(tryb_pracy==3){ // Urandom
        wejscie = fopen("/dev/urandom", "r");
        if(wejscie == NULL){
            perror(" -> [P1] Błąd /dev/urandom");
            exit(1);
        }
        printf(" -> [P1] Tryb ciągły /dev/urandom uruchomiony.\n");
    }

    // Pętla główna
    while (czy_dzialac && (odczytane_bajty = read(fileno(wejscie), bufor, sizeof(bufor))) > 0){
        
        // Obsługa pauzy
        while(czy_wstrzymany && czy_dzialac) { 
            usleep(100000); // 100ms sleep w pauzie, żeby nie obciążać CPU
        }
        if(!czy_dzialac) break; 

        // Usuwanie nowej linii tylko w trybie interaktywnym/plikowym (nie binarnym)
        if (tryb_pracy != 3 && odczytane_bajty > 0 && bufor[odczytane_bajty-1] == '\n'){
            odczytane_bajty--;
        }
        if(odczytane_bajty == 0) continue;

        // Konwersja
        if (tryb_hex == 1){
            to_hex((char*)bufor, odczytane_bajty, hex_bufor);
        }
        else{
            memcpy(hex_bufor, bufor, odczytane_bajty);
            hex_bufor[odczytane_bajty] = '\0';
        }

        // Synchronizacja i zapis
        // Czekamy aż P2 zwolni pamięć (status == 0)
        while(dzielona_pamiec->status == 1 && czy_dzialac){
            usleep(100); // Krótkie czekanie (Active Wait z usypianiem)
        }
        if(!czy_dzialac) break; 

        // Kopiowanie do pamięci współdzielonej
        strncpy(dzielona_pamiec->dane, hex_bufor, SHARED_SIZE-1);
        dzielona_pamiec->dane[SHARED_SIZE-1]='\0';

        if(tryb_hex == 1) dzielona_pamiec->wielkosc_danych = strlen(hex_bufor);
        else dzielona_pamiec->wielkosc_danych = odczytane_bajty;

        // Ustawienie flagi dla P2
        dzielona_pamiec->status = 1;
        
        // Brak sleep(1) - maksymalna prędkość!
    }

    // Wyjście z programu
    while(dzielona_pamiec->status == 1 && czy_dzialac) usleep(1000);

    if(czy_dzialac) {
        dzielona_pamiec->czy_koniec = 1;
        dzielona_pamiec->status = 1;
    }

    if(tryb_pracy != 1 && wejscie != NULL) fclose(wejscie);
    exit(0);
}

// P2: Shared Memory -> Pipe
void proces_2() {
    signal(SIGTERM, obsluga_sygnalow); 
    signal(SIGUSR1, obsluga_sygnalow);
    signal(SIGUSR2, obsluga_sygnalow);
    signal(SIGALRM, obsluga_sygnalow);

    close(potok[0]); 

    while(czy_dzialac){
        while(czy_wstrzymany && czy_dzialac) usleep(100000);
        if (!czy_dzialac) break;

        // Czekamy na dane od P1 (status=1)
        while(dzielona_pamiec->status == 0 && czy_dzialac){
            usleep(100); 
        }

        if(dzielona_pamiec->czy_koniec == 1) break;

        // Przekazanie do potoku
        int len = dzielona_pamiec->wielkosc_danych;
        if (len > 0){
            if (write(potok[1], dzielona_pamiec->dane, len) == -1){
                perror(" -> [P2] Błąd zapisu do potoku");
                break;
            }
        }

        // Zwolnienie pamięci dla P1
        dzielona_pamiec->status = 0;
    }

    close(potok[1]);
    exit(0);
}

// P3: Pipe -> Ekran
void proces_3() {
    signal(SIGTERM, obsluga_sygnalow);
    signal(SIGUSR1, obsluga_sygnalow);
    signal(SIGUSR2, obsluga_sygnalow);
    signal(SIGALRM, obsluga_sygnalow);

    close(potok[1]);
    
    char bufor_pipe[SHARED_SIZE];
    ssize_t bajty; 
    int licznik_jednostek = 0; 

    // printf("\n -> [P3] DANE WYJŚCIOWE:\n"); // Opcjonalne

    while (czy_dzialac && (bajty = read(potok[0], bufor_pipe, sizeof(bufor_pipe)-1)) > 0){
        
        while (czy_wstrzymany && czy_dzialac) usleep(100000);
        if (!czy_dzialac) break;

        bufor_pipe[bajty] = '\0'; 

        if (tryb_hex == 1){
            for(int i=0; i<bajty; i+=2){ 
                if (i+1 < bajty){ 
                    printf("%c%c ", bufor_pipe[i], bufor_pipe[i+1]); // Spacja po każdej parze
                    licznik_jednostek++;

                    if (licznik_jednostek >= 15){ // Wymóg: 15 jednostek w wierszu
                        printf("\n");
                        licznik_jednostek = 0;
                    }
                }
            }
        }
        else{ 
            printf("%s", bufor_pipe);
            fflush(stdout); // Ważne przy raw
            licznik_jednostek = 0;
        }
        
        // fflush(stdout); // Zbyt częste flushowanie zwalnia, ale zapewnia płynność
    }
    
    // Dopisz enter na końcu, jeśli linia nie była pełna
    if (licznik_jednostek != 0) printf("\n");

    close(potok[0]); 
    exit(0);
}

// Obsługa sygnałów
void obsluga_sygnalow(int sygn){
    pid_t my_pid = getpid();
    pid_t p1 = dzielona_pamiec->pid_p1;
    pid_t p2 = dzielona_pamiec->pid_p2;
    pid_t p3 = dzielona_pamiec->pid_p3;

    if (sygn == SIGTERM){ 
        if(czy_dzialac == 1) { 
            czy_dzialac = 0;
            if (my_pid != p1) kill(p1, SIGTERM);
            if (my_pid != p2) kill(p2, SIGTERM);
            if (my_pid != p3) kill(p3, SIGTERM);
        }
    }
    else if (sygn == SIGUSR1) { 
        if (czy_wstrzymany == 0) { 
            czy_wstrzymany = 1;
            if (my_pid != p1) kill(p1, SIGUSR1);
            if (my_pid != p2) kill(p2, SIGUSR1);
            if (my_pid != p3) kill(p3, SIGUSR1);
        }
    } 
    else if (sygn == SIGUSR2) { 
        if (czy_wstrzymany == 1) { 
            czy_wstrzymany = 0;
            if (my_pid != p1) kill(p1, SIGUSR2);
            if (my_pid != p2) kill(p2, SIGUSR2);
            if (my_pid != p3) kill(p3, SIGUSR2);
        }
    }
    else if (sygn == SIGALRM){
        tryb_hex = !tryb_hex;
        if (my_pid != p1) kill(p1, SIGALRM);
        if (my_pid != p2) kill(p2, SIGALRM);
        if (my_pid != p3) kill(p3, SIGALRM);
    }
}

pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa){
    pid_t pid = fork();
    if(pid == 0){
        funkcja_procesu();
        exit(0); 
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
        fprintf(stderr, " -i           : Tryb interaktywny\n");
        fprintf(stderr, " -f <plik>    : Odczyt z pliku\n");
        fprintf(stderr, " -r           : Odczyt z /dev/urandom\n");
        return -1;
    }
    if (strcmp(argv[1], "-i") == 0) tryb_pracy = 1;
    else if(strcmp(argv[1], "-f") == 0){
        if (argc < 3) return -1;
        tryb_pracy = 2; 
        sciezka_do_pliku = argv[2];
    }
    else if(strcmp(argv[1], "-r") == 0) tryb_pracy = 3;
    else return -1;
    return 0; 
}

void to_hex(const char *ascii, size_t len, char *hex){
    const char hex_map[] = "0123456789ABCDEF";
    for (size_t i=0; i<len; i++){
        unsigned char bajt = (unsigned char)ascii[i];
        hex[i*2] = hex_map[bajt >> 4];
        hex[i*2 + 1] = hex_map[bajt & 0x0F];
    }
    hex[len*2] = '\0'; 
}