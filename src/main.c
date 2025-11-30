

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <errno.h>

#define BUFF_SIZE 5
#define NFILES 50        //número de arquivos que P deve processar; ajuste se quiser
#define MAT_N 10         //ordem das matrizes
#define MAX_NAME 200

//Define a quantidade de threads de cada etapa
#define N_P 1
#define N_CP1 5
#define N_CP2 4
#define N_CP3 3
#define N_C 1

typedef struct {
    char nome[MAX_NAME];
    double A[MAT_N][MAT_N];
    double B[MAT_N][MAT_N];
    double C[MAT_N][MAT_N];
    double V[MAT_N];
    double E;
} S;

typedef struct {
    S* buf[BUFF_SIZE];
    int in;
    int out;
    sem_t full;
    sem_t empty;
    sem_t mutex;
} buffer_t;

buffer_t shared[4];

void init_buffers() {
    for (int i = 0; i < 4; i++) {
        shared[i].in = 0;
        shared[i].out = 0;
        sem_init(&shared[i].full, 0, 0);
        sem_init(&shared[i].empty, 0, BUFF_SIZE);
        sem_init(&shared[i].mutex, 0, 1);
        for (int j = 0; j < BUFF_SIZE; j++) shared[i].buf[j] = NULL;
    }
}


void buffer_push(int idx, S* item) {
    sem_wait(&shared[idx].empty);
    sem_wait(&shared[idx].mutex);
    shared[idx].buf[shared[idx].in] = item;
    shared[idx].in = (shared[idx].in + 1) % BUFF_SIZE;
    sem_post(&shared[idx].mutex);
    sem_post(&shared[idx].full);
}

S* buffer_pop(int idx) {
    sem_wait(&shared[idx].full);
    sem_wait(&shared[idx].mutex);
    S* item = shared[idx].buf[shared[idx].out];
    shared[idx].out = (shared[idx].out + 1) % BUFF_SIZE;
    sem_post(&shared[idx].mutex);
    sem_post(&shared[idx].empty);
    return item;
}


int read_matrix_from_file(FILE* f, double M[MAT_N][MAT_N]) {
    char line[4096];
    for (int i = 0; i < MAT_N; i++) {
        if (!fgets(line, sizeof(line), f)) return -1;

     
        for (char *p = line; *p; ++p) if (*p == ',') *p = ' ';
        char *ptr = line;
        for (int j = 0; j < MAT_N; j++) {
            while (*ptr == ' ' || *ptr == '\t') ptr++; 
            char *end;
            double val = strtod(ptr, &end);
            if (ptr == end) return -1;
            M[i][j] = val;
            ptr = end;
        }
    }
    return 0;
}

//Função Producer: lê arquivo entrada.in com NFILES nomes (um por linha)
void* producer_thread(void* arg) {
    const char* listfile = (const char*) arg; 
    FILE* fl = fopen(listfile, "r");
    if (!fl) {
        perror("Producer: fopen entrada.in");
        return NULL;
    }

    char fname[512];
    int count = 0;
    while (count < NFILES && fgets(fname, sizeof(fname), fl)) {
        char *nl = strchr(fname, '\n');
        if (nl) *nl = '\0';
        if (strlen(fname) == 0) continue;
        S *s = calloc(1, sizeof(S));
        snprintf(s->nome, MAX_NAME, "%s", fname);

        FILE* fin = fopen(fname, "r");
        if (!fin) {
            fprintf(stderr, "Producer: nao conseguiu abrir %s\n", fname);
            free(s);
            continue;
        }

        if (read_matrix_from_file(fin, s->A) != 0) {
            fprintf(stderr, "Producer: erro lendo A em %s\n", fname);
            fclose(fin); free(s); continue;
        }
        if (read_matrix_from_file(fin, s->B) != 0) {
            fprintf(stderr, "Producer: erro lendo B em %s\n", fname);
            fclose(fin); free(s); continue;
        }
        fclose(fin);

        //coloca ponteiro no shared[0]
        buffer_push(0, s);
        fflush(stdout);
    }
    fclose(fl);
    printf("[P] finalizou. Total lidos = %d\n", count);
    for (int i = 0; i < N_CP1; i++) {
        buffer_push(0, NULL); 
    } 
    return NULL;
}

//CP1: consume shared[0], calcula C = A*B, push em shared[1]
void* cp1_thread(void* arg) {
    int tid = *(int*)arg;
    (void)tid;
    while (1) {
        S* s = buffer_pop(0);
         if (!s) { 
            for (int i = 0; i < N_CP2; i++) {
                buffer_push(1, NULL);
            } break;
        }
        // multiplicacao matricial
        for (int i = 0; i < MAT_N; i++) {
            for (int j = 0; j < MAT_N; j++) {
                double sum = 0.0;
                for (int k = 0; k < MAT_N; k++)
                    sum += s->A[i][k] * s->B[k][j];
                s->C[i][j] = sum;
            }
        }
        printf("[CP1_%d] Processou %s -> calculou C\n", tid, s->nome);
        fflush(stdout);
        buffer_push(1, s);
    }
    return NULL;
}

//CP2: consume shared[1], calcula V = soma das colunas de C, push em shared[2]
void* cp2_thread(void* arg) {
    int tid = *(int*)arg;
    (void)tid;
    int sentinelas_necessarios = N_CP1;

    while (1) {
        S* s = buffer_pop(1);
        if (!s) { 
            sentinelas_necessarios--;
            if (sentinelas_necessarios == 0) {
                for (int i = 0; i < N_CP3; i++) {
                    buffer_push(2, NULL);
                } 
                break;
            }
            continue;
        }

        for (int j = 0; j < MAT_N; j++) {
            double sum = 0.0;
            for (int i = 0; i < MAT_N; i++) sum += s->C[i][j];
            s->V[j] = sum;
        }
        printf("[CP2_%d] Processou %s -> calculou V\n", tid, s->nome);
        fflush(stdout);
        buffer_push(2, s);
    }
    return NULL;
}

//CP3: consume shared[2], calcula E = soma de V, push em shared[3]
void* cp3_thread(void* arg) {
    int tid = *(int*)arg;
    (void)tid;
    int sentinelas_necessarios = N_CP2;
    while (1) {
        S* s = buffer_pop(2);
        if (!s) { 
            sentinelas_necessarios--;
            if (sentinelas_necessarios == 0) {
                for (int i = 0; i < N_C; i++) {
                    buffer_push(3, NULL);
            } 
            break; 
        }
        continue;
    }
        double sum = 0.0;
        for (int i = 0; i < MAT_N; i++) sum += s->V[i]; 
            s->E = sum;
            printf("[CP3_%d] Processou %s -> calculou E=%.6f\n", tid, s->nome, s->E);
            fflush(stdout);
            buffer_push(3, s);
        } 

return NULL; 
}

//Função Consumer final: escreve saida.out e faz contagem; quando chega a NFILES, termina
void* consumer_thread(void* arg) {
    const char* outpath = (const char*)arg;
    FILE* fout = fopen(outpath, "w");
    if (!fout) {
        perror("Consumer: fopen saida.out");
        return NULL;
    }
    int local_count = 0;
    while (local_count < NFILES) {
        S* s = buffer_pop(3);
        if (!s) continue;

        //Formatação dos prints de saída
        fprintf(fout, "================================\n");
        fprintf(fout, "Entrada: %s;\n", s->nome);
        fprintf(fout, "--------------------------\n");
        fprintf(fout, "A\n");
        for (int i = 0; i < MAT_N; i++) {
            for (int j = 0; j < MAT_N; j++) {
                fprintf(fout, "%.6f", s->A[i][j]);
                if (j < MAT_N-1) fprintf(fout, " ");
            }
            fprintf(fout, "\n");
        }
        fprintf(fout, "--------------------------\n");
        fprintf(fout, "B\n");
        for (int i = 0; i < MAT_N; i++) {
            for (int j = 0; j < MAT_N; j++) {
                fprintf(fout, "%.6f", s->B[i][j]);
                if (j < MAT_N-1) fprintf(fout, " ");
            }
            fprintf(fout, "\n");
        }
        fprintf(fout, "--------------------------\n");
        fprintf(fout, "C\n");
        for (int i = 0; i < MAT_N; i++) {
            for (int j = 0; j < MAT_N; j++) {
                fprintf(fout, "%.6f", s->C[i][j]);
                if (j < MAT_N-1) fprintf(fout, " ");
            }
            fprintf(fout, "\n");
        }
        fprintf(fout, "--------------------------\n");
        fprintf(fout, "V\n");
        for (int i = 0; i < MAT_N; i++) {
            fprintf(fout, "%.6f\n", s->V[i]);
        }
        fprintf(fout, "--------------------------\n");
        fprintf(fout, "E\n");
        fprintf(fout, "%.6f\n", s->E);
        fprintf(fout, "================================\n\n");
        fflush(fout);

        free(s); // libera a struct alocada por P
        local_count++;
        printf("[C] Escreveu %s (contador C = %d)\n", s->nome, local_count);
        fflush(stdout);
    }
    fclose(fout);
    printf("[C] Processou todos os arquivos (%d). Finalizando C.\n", local_count);
    return NULL;
}


void cleanup() {
    for (int i = 0; i < 4; i++) {
        sem_destroy(&shared[i].full);
        sem_destroy(&shared[i].empty);
        sem_destroy(&shared[i].mutex);
    }
}

   //espera: ./program entrada.in output/saida.out
int main(int argc, char** argv) {
    const char* listfile = "input/entrada.in";
    const char* outpath = "output/saida.out";
    if (argc >= 2) listfile = argv[1];
    if (argc >= 3) outpath = argv[2];

    init_buffers();

    pthread_t tP, tC;
    pthread_t tCP1[N_CP1], tCP2[N_CP2], tCP3[N_CP3];
    int ids_cp1[N_CP1], ids_cp2[N_CP2], ids_cp3[N_CP3];

    //cria threads
    if (pthread_create(&tP, NULL, producer_thread, (void*)listfile) != 0) {
        perror("pthread_create P");
        exit(1);
    }

    for (int i = 0; i < N_CP1; i++) { ids_cp1[i] = i; pthread_create(&tCP1[i], NULL, cp1_thread, &ids_cp1[i]); }
    for (int i = 0; i < N_CP2; i++) { ids_cp2[i] = i; pthread_create(&tCP2[i], NULL, cp2_thread, &ids_cp2[i]); }
    for (int i = 0; i < N_CP3; i++) { ids_cp3[i] = i; pthread_create(&tCP3[i], NULL, cp3_thread, &ids_cp3[i]); }

    if (pthread_create(&tC, NULL, consumer_thread, (void*)outpath) != 0) {
        perror("pthread_create C");
        exit(1);
    }

    //Pai espera a thread C terminar
    pthread_join(tC, NULL);
    printf("[main] C terminou. Liberando CP threads e encerrando.\n");

    pthread_join(tP, NULL);
    for (int i = 0; i < N_CP1; i++) pthread_join(tCP1[i], NULL);
    for (int i = 0; i < N_CP2; i++) pthread_join(tCP2[i], NULL);
    for (int i = 0; i < N_CP3; i++) pthread_join(tCP3[i], NULL);
    
    cleanup();
    printf("[main] finalizado e limpou recursos\n");
    return 0;
}
