#define _POSIX_C_SOURCE 200809L 
#define _DEFAULT_SOURCE         

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h> 
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <sys/ipc.h> 
#include <sys/shm.h> 
#include <sys/sem.h> // Biblioteka semaforów
#include <errno.h> 

#pragma region ZMIENNE GLOBALNE dla KOMUNIKACJI

#define SHARED_SIZE 4096 

// Definicje dla semaforów
#define SEM_EMPTY 0 // Semafor pustości (P1 czeka na to, by zapisać)
#define SEM_FULL  1 // Semafor pełności (P2 czeka na to, by odczytać)

// Struktura w pamieci wspolnej
typedef struct{
    volatile int czy_koniec; // Nadal potrzebne, by P2 wiedział kiedy wyjść

    pid_t pid_p1;
    pid_t pid_p2;
    pid_t pid_p3;

    int wielkosc_danych;
    char dane[SHARED_SIZE]; 
} DaneWspolne;

DaneWspolne *dzielona_pamiec = NULL;
int shm_id = 0;
int sem_id = 0; // ID zbioru semaforów

// Unia wymagana przez semctl
union semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

#pragma endregion

#pragma region ZMIENNE GLOBALNE
pid_t P1 = 0;
pid_t P2 = 0;
pid_t P3 = 0;

volatile int czy_dzialac = 1; 
volatile int czy_wstrzymany = 0; 
volatile int tryb_hex = 1; 

int tryb_pracy = 0;
char *sciezka_do_pliku = NULL;

int potok[2]; 
#pragma endregion

#pragma region DEKLARACJE FUNKCJI
pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa);
int konfiguracja_trybow(int argc, char *argv[]);
void to_hex(const char *ascii, size_t len, char *hex);

void obsluga_sygnalow_extended(int sygn, siginfo_t *info, void *context);
void zarejestruj_sygnaly(); // Deklaracja

// Funkcje obsługi semaforów
void sem_lock(int sem_num);   
void sem_unlock(int sem_num); 

void proces_1();
void proces_2();
void proces_3();
#pragma endregion

int main(int argc, char *argv[]){
    if (konfiguracja_trybow(argc, argv) != 0) return 1;

    // 1. TWORZENIE PAMIECI WSPOLDZIELONEJ
    shm_id = shmget(IPC_PRIVATE, sizeof(DaneWspolne), 0666 | IPC_CREAT);
    if(shm_id < 0){ perror("Blad shmget"); return 1; }

    dzielona_pamiec = (DaneWspolne*)shmat(shm_id, NULL, 0);
    if (dzielona_pamiec == (DaneWspolne*)-1){ perror("Blad shmat"); return 1; }

    dzielona_pamiec->czy_koniec = 0;
    memset(dzielona_pamiec->dane, 0, SHARED_SIZE);

    // 2. TWORZENIE SEMAFORÓW (2 semafory w zestawie)
    sem_id = semget(IPC_PRIVATE, 2, 0666 | IPC_CREAT);
    if (sem_id < 0) { perror("Blad semget"); return 1; }

    // Inicjalizacja semaforów
    union semun arg;
    arg.val = 1;
    semctl(sem_id, SEM_EMPTY, SETVAL, arg); // 1 = Wolne miejsce
    arg.val = 0;
    semctl(sem_id, SEM_FULL, SETVAL, arg);  // 0 = Brak danych

    // 3. TWORZENIE POTOKU
    if (pipe(potok) == -1){ perror("[MAIN] Błąd pipe"); return 1; }

    // 4. TWORZENIE PROCESOW
    P1 = utworz_proces(proces_1, "P1");
    P2 = utworz_proces(proces_2, "P2");
    P3 = utworz_proces(proces_3, "P3");

    printf("[MAIN] System uruchomiony. PIDy: P1=%d, P2=%d, P3=%d\n", P1, P2, P3);
    
    FILE *plik_pid = fopen("pidy_procesow.txt", "w");
    if (plik_pid) {
        fprintf(plik_pid, "%d %d %d", P1, P2, P3);
        fclose(plik_pid);
    }

    dzielona_pamiec->pid_p1 = P1;
    dzielona_pamiec->pid_p2 = P2;
    dzielona_pamiec->pid_p3 = P3;

    close(potok[0]);
    close(potok[1]);

    // Oczekiwanie na koniec
    while (wait(NULL) > 0); 

    printf("[MAIN] Wszystkie procesy zakończone. Sprzątanie...\n");
    unlink("pidy_procesow.txt"); 

    // Sprzątanie
    shmdt(dzielona_pamiec);         
    shmctl(shm_id, IPC_RMID, NULL); 
    semctl(sem_id, 0, IPC_RMID); 
    
    return 0;
}

void zarejestruj_sygnaly() {
    struct sigaction sa;
    sa.sa_sigaction = obsluga_sygnalow_extended;
    sa.sa_flags = SA_SIGINFO; 
    sigemptyset(&sa.sa_mask);

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
}

// Implementacja operacji na semaforach
void sem_lock(int sem_num) {
    struct sembuf sb;
    sb.sem_num = sem_num;
    sb.sem_op = -1; 
    sb.sem_flg = 0;
    
    while (semop(sem_id, &sb, 1) == -1) {
        if (errno == EINTR && czy_dzialac) continue;
        else break; 
    }
}

void sem_unlock(int sem_num) {
    struct sembuf sb;
    sb.sem_num = sem_num;
    sb.sem_op = 1;
    sb.sem_flg = 0;
    
    while (semop(sem_id, &sb, 1) == -1) {
        if (errno == EINTR && czy_dzialac) continue;
        else break; 
    }
}

// P1: Producent 
void proces_1() {
    zarejestruj_sygnaly(); 
    while(dzielona_pamiec->pid_p3 == 0) usleep(1000); 

    FILE *wejscie = NULL;
    ssize_t odczytane_bajty;
    unsigned char bufor[1024]; 
    char hex_bufor[(sizeof(bufor) * 2) + 1]; 

    if(tryb_pracy==1){ 
        wejscie = stdin;
        printf(" -> [P1] Tryb Interaktywny. Wpisuj tekst...\n");    
    }
    else if(tryb_pracy==2){ 
        wejscie = fopen(sciezka_do_pliku, "r");
        if(!wejscie) exit(1);
    }
    else if(tryb_pracy==3){ 
        wejscie = fopen("/dev/urandom", "r");
        if(!wejscie) exit(1);
    }

    while (czy_dzialac){
        odczytane_bajty = read(fileno(wejscie), bufor, sizeof(bufor));
        if (odczytane_bajty < 0) {
            if (errno == EINTR) continue; 
            else break; 
        }
        if (odczytane_bajty == 0) break; 

        while(czy_wstrzymany && czy_dzialac) usleep(100000);
        if(!czy_dzialac) break; 

        // Usuwanie ENTER w trybie Interaktywnym
        if (tryb_pracy == 1 && odczytane_bajty > 0 && bufor[odczytane_bajty-1] == '\n') {
            odczytane_bajty--;
        }
        if (odczytane_bajty == 0) continue; 

        if (tryb_hex == 1){
            to_hex((char*)bufor, odczytane_bajty, hex_bufor);
        }
        else{
            memcpy(hex_bufor, bufor, odczytane_bajty);
            hex_bufor[odczytane_bajty] = '\0';
        }

        // Czekaj na wolne miejsce (SEM_EMPTY)
        sem_lock(SEM_EMPTY);
        
        if(!czy_dzialac) {
            sem_unlock(SEM_FULL); 
            break; 
        }

        strncpy(dzielona_pamiec->dane, hex_bufor, SHARED_SIZE-1);
        dzielona_pamiec->dane[SHARED_SIZE-1]='\0';

        if(tryb_hex == 1) dzielona_pamiec->wielkosc_danych = strlen(hex_bufor);
        else dzielona_pamiec->wielkosc_danych = odczytane_bajty;

        // Sygnalizuj dane (SEM_FULL)
        sem_unlock(SEM_FULL);
    }

    if (czy_dzialac) {
        sem_lock(SEM_EMPTY);
        dzielona_pamiec->czy_koniec = 1;
        sem_unlock(SEM_FULL);
    } else {
        dzielona_pamiec->czy_koniec = 1;
        sem_unlock(SEM_FULL);
    }

    if(tryb_pracy != 1 && wejscie != NULL) fclose(wejscie);
    exit(0);
}

// P2: Pośrednik
void proces_2() {
    zarejestruj_sygnaly(); 
    close(potok[0]); 

    while(czy_dzialac){
        while(czy_wstrzymany && czy_dzialac) usleep(100000);
        if (!czy_dzialac) break;

        // Czekaj na dane (SEM_FULL)
        sem_lock(SEM_FULL);
        
        if(dzielona_pamiec->czy_koniec == 1) {
             sem_unlock(SEM_EMPTY); 
             break;
        }
        if (!czy_dzialac) break;

        int len = dzielona_pamiec->wielkosc_danych;
        if (len > 0){
            ssize_t written_total = 0;
            while (written_total < len) {
                ssize_t ret = write(potok[1], dzielona_pamiec->dane + written_total, len - written_total);
                if (ret == -1) {
                    if (errno == EINTR) continue; 
                    else {
                        perror(" -> [P2] Błąd zapisu");
                        czy_dzialac = 0; 
                        break;
                    }
                }
                written_total += ret;
            }
        }

        // Zwolnij bufor (SEM_EMPTY)
        sem_unlock(SEM_EMPTY);
    }

    close(potok[1]);
    exit(0);
}

// P3: Konsument
void proces_3() {
    zarejestruj_sygnaly(); 
    close(potok[1]);
    
    char bufor_pipe[SHARED_SIZE];
    ssize_t bajty; 
    int licznik_jednostek = 0; 

    while (czy_dzialac){
        bajty = read(potok[0], bufor_pipe, sizeof(bufor_pipe)-1);

        if (bajty < 0) {
            if (errno == EINTR) continue; 
            else break; 
        }
        if (bajty == 0) break; 

        while (czy_wstrzymany && czy_dzialac) usleep(100000);
        if (!czy_dzialac) break;

        bufor_pipe[bajty] = '\0'; 

        if (tryb_hex == 1){
            for(int i=0; i<bajty; i+=2){ 
                if (i+1 < bajty){ 
                    printf("%c%c ", bufor_pipe[i], bufor_pipe[i+1]); 
                    licznik_jednostek++;
                    if (licznik_jednostek >= 15){ 
                        printf("\n");
                        licznik_jednostek = 0;
                    }
                }
            }
            fflush(stdout); 
            
            if (tryb_pracy == 1 && licznik_jednostek != 0) {
                printf("\n");
                licznik_jednostek = 0;
            }
        }
        else{ 
            // Tryb RAW
            fwrite(bufor_pipe, 1, bajty, stdout);
            
            // --- POPRAWKA ---
            // Ponieważ P1 usunął ENTER, musimy go dodać sztucznie w P3,
            // ale TYLKO w trybie interaktywnym, żeby kursor spadł do nowej linii.
            if (tryb_pracy == 1) {
                printf("\n");
            }
            // ----------------
            
            fflush(stdout); 
            licznik_jednostek = 0;
        }
    }
    
    if (licznik_jednostek != 0) printf("\n");
    close(potok[0]); 
    exit(0);
}

// Obsługa sygnałów
void obsluga_sygnalow_extended(int sygn, siginfo_t *info, void *context){
    pid_t nadawca = info->si_pid;
    pid_t my_pid = getpid();
    pid_t p1 = dzielona_pamiec->pid_p1;
    pid_t p2 = dzielona_pamiec->pid_p2;
    pid_t p3 = dzielona_pamiec->pid_p3;

    int czy_od_programu = (nadawca == p1 || nadawca == p2 || nadawca == p3);

    if (sygn == SIGTERM){ 
        if(czy_dzialac == 1) czy_dzialac = 0;
    }
    else if (sygn == SIGUSR1) { 
        if (czy_wstrzymany == 0) czy_wstrzymany = 1;
    } 
    else if (sygn == SIGUSR2) { 
        if (czy_wstrzymany == 1) czy_wstrzymany = 0;
    }
    else if (sygn == SIGALRM){
        tryb_hex = !tryb_hex;
    }

    if (!czy_od_programu) {
        if (sygn == SIGTERM) {
            if (my_pid != p1) kill(p1, SIGTERM);
            if (my_pid != p2) kill(p2, SIGTERM);
            if (my_pid != p3) kill(p3, SIGTERM);
        } else {
            if (my_pid != p1) kill(p1, sygn);
            if (my_pid != p2) kill(p2, sygn);
            if (my_pid != p3) kill(p3, sygn);
        }
    }
}

pid_t utworz_proces(void (*funkcja_procesu)(), const char* nazwa){
    pid_t pid = fork();
    if(pid == 0){ funkcja_procesu(); exit(0); }
    else if(pid < 0){ perror(nazwa); exit(1); }
    return pid;
}

int konfiguracja_trybow(int argc, char *argv[]){
    if (argc < 2) return -1;
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