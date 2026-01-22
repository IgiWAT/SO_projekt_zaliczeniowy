#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h> //obsługa nazw zmiennych 


// Struktura do przechowywania PID-ów
typedef struct {
    pid_t p1;
    pid_t p2;
    pid_t p3;
} Procesy;

Procesy pidy = {0, 0, 0};

// Funkcja wczytująca PIDy z pliku (dla wygody)
int wczytaj_pidy() {
    FILE *plik = fopen("pidy_procesow.txt", "r");
    if (plik == NULL) {
        printf("[MANAGER] Nie znaleziono pliku 'pidy_procesow.txt'.\n");
        printf("[MANAGER] Uruchom najpierw program glowny (main).\n");
        return 0;
    }
    if (fscanf(plik, "%d %d %d", &pidy.p1, &pidy.p2, &pidy.p3) != 3) {
        printf("[MANAGER] Błąd formatu pliku z PIDami.\n");
        fclose(plik);
        return 0;
    }
    fclose(plik);
    return 1;
}

// Funkcja pomocnicza do pobierania PID od użytkownika (jeśli plik nie zadziała)
void reczne_wprowadzanie() {
    printf("Podaj PID procesu P1: ");
    scanf("%d", &pidy.p1);
    printf("Podaj PID procesu P2: ");
    scanf("%d", &pidy.p2);
    printf("Podaj PID procesu P3: ");
    scanf("%d", &pidy.p3);
}

int main() {
    int wybor_sygnalu = 0;
    int wybor_procesu = 0;
    pid_t cel = 0;

    printf("--- MANAGER SYGNAŁÓW ---\n");
    
    // Próba automatycznego wczytania
    if (!wczytaj_pidy()) {
        printf("Czy chcesz wprowadzić PIDy ręcznie? (1-Tak, 0-Wyjście): ");
        int decyzja;
        scanf("%d", &decyzja);
        if (decyzja == 1) reczne_wprowadzanie();
        else return 0;
    }

    printf("[INFO] Załadowano procesy: P1=%d, P2=%d, P3=%d\n", pidy.p1, pidy.p2, pidy.p3);

    while (1) {
        printf("\n================ MENU ==================\n");
        printf("1. Wyślij STOP (Koniec - S1) [SIGTERM]\n");
        printf("2. Wyślij PAUZA (Wstrzymaj - S2) [SIGUSR1]\n");
        printf("3. Wyślij START (Wznów - S3) [SIGUSR2]\n");
        printf("4. Przełącz tryb HEX/RAW (S4) [SIGALRM]\n");
        printf("0. Wyjście z Managera\n");
        printf("========================================\n");
        printf("Wybierz akcję: ");
        scanf("%d", &wybor_sygnalu);

        if (wybor_sygnalu == 0) break;
        if (wybor_sygnalu < 1 || wybor_sygnalu > 4) {
            printf("Niepoprawny wybór!\n");
            continue;
        }

        printf("\nDo którego procesu wysłać sygnał?\n");
        printf("1. Proces P1 (Producent / Konwersja)\n");
        printf("2. Proces P2 (Pośrednik)\n");
        printf("3. Proces P3 (Konsument / Ekran)\n");
        printf("Wybór: ");
        scanf("%d", &wybor_procesu);

        switch (wybor_procesu) {
            case 1: cel = pidy.p1; break;
            case 2: cel = pidy.p2; break;
            case 3: cel = pidy.p3; break;
            default: cel = 0; printf("Niepoprawny proces!\n");
        }

        if (cel != 0) {
            int sygnal = 0;
            switch (wybor_sygnalu) {
                case 1: sygnal = SIGTERM; break;
                case 2: sygnal = SIGUSR1; break;
                case 3: sygnal = SIGUSR2; break;
                case 4: sygnal = SIGALRM; break;
            }

            // Wysyłanie sygnału
            if (kill(cel, sygnal) == 0) {
                printf(" -> Wysłano sygnał %d do procesu %d.\n", sygnal, cel);
            } else {
                perror(" -> Błąd wysyłania sygnału");
                printf("Czy procesy na pewno istnieją?\n");
            }
        }
    }

    return 0;
}