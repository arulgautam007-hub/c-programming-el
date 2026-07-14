/*
 * tetris_engine.c  --  headless JSON engine
 * Compile: gcc tetris_engine.c -o tetris_engine.exe
 *
 * Protocol:
 *   stdin  <- one JSON command per line
 *   stdout -> one JSON state per line after every command
 *
 * Commands:
 *   {"cmd":"start"}
 *   {"cmd":"tick"}
 *   {"cmd":"key","key":"left"}   left/right/rotate/drop/bomb/mult
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── dimensions ─────────────────────────────────────────────────────────────── */
#define COLS  10
#define ROWS  20
#define EMPTY 0
#define BLOCK 1
#define TRUE  1
#define FALSE 0

/* ── shapes (7 tetrominoes, each stored with a color id 1-7) ─────────────── */
static int SHAPES[7][4][4] = {
    {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}, /* I */
    {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, /* O */
    {{0,0,0,0},{0,1,0,0},{1,1,1,0},{0,0,0,0}}, /* T */
    {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, /* S */
    {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, /* Z */
    {{0,0,0,0},{0,1,0,0},{0,1,1,1},{0,0,0,0}}, /* J */
    {{0,0,0,0},{0,1,1,1},{0,1,0,0},{0,0,0,0}}  /* L */
};

/* ── game state ─────────────────────────────────────────────────────────────── */
static int  grid[ROWS][COLS];       /* 0=empty, 1-7=locked piece color */
static int  cur[4][4];              /* current piece shape */
static int  curColor;               /* current piece color id (1-7) */
static int  cx, cy;                 /* current piece position */
static int  next[3][4][4];          /* next 3 pieces */
static int  nextColor[3];           /* their colors */
static int  score, bestScore;
static int  bombCount, multCount;
static int  multiplier, multClearLeft;
static int  lastBombScore, lastMultScore;
static int  gameOver;
static int  started;
static int  tickAccum;              /* counts ticks for speed control */
static int  level;                  /* increases every 10 lines */
static int  linesCleared;

/* ── helpers ────────────────────────────────────────────────────────────────── */
static void copyShape(int dst[4][4], int src[4][4]){
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) dst[i][j]=src[i][j];
}

static void randomPiece(int shape[4][4], int *color){
    int id = rand()%7;
    copyShape(shape, SHAPES[id]);
    *color = id+1;
}

static int collides(int shape[4][4], int px, int py){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        if(!shape[r][c]) continue;
        int gx=px+c, gy=py+r;
        if(gx<0||gx>=COLS||gy>=ROWS) return TRUE;
        if(gy>=0 && grid[gy][gx]) return TRUE;
    }
    return FALSE;
}

static void lockPiece(){
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        if(cur[r][c]){
            int gy=cy+r, gx=cx+c;
            if(gy>=0 && gy<ROWS && gx>=0 && gx<COLS)
                grid[gy][gx]=curColor;
        }
    }
}

static void checkLines(){
    int cleared=0;
    for(int r=ROWS-1;r>=0;r--){
        int full=1;
        for(int c=0;c<COLS;c++) if(!grid[r][c]){full=0;break;}
        if(full){
            /* shift everything above down */
            for(int rr=r;rr>0;rr--)
                for(int c=0;c<COLS;c++)
                    grid[rr][c]=grid[rr-1][c];
            for(int c=0;c<COLS;c++) grid[0][c]=0;
            cleared++;
            r++; /* recheck this row */
        }
    }
    if(cleared>0){
        linesCleared+=cleared;
        level=linesCleared/10;
        int base=cleared==1?5:cleared==2?12:cleared==3?20:30;
        score+=base*multiplier;
        if(multiplier>1){
            multClearLeft-=cleared;
            if(multClearLeft<=0){multiplier=1;multClearLeft=0;}
        }
        while(score-lastBombScore>=5000){bombCount++;lastBombScore+=5000;}
        while(score-lastMultScore>=10000){multCount++;lastMultScore+=10000;}
        if(score>bestScore) bestScore=score;
    }
}

static void spawnNext(){
    copyShape(cur,next[0]); curColor=nextColor[0];
    copyShape(next[0],next[1]); nextColor[0]=nextColor[1];
    copyShape(next[1],next[2]); nextColor[1]=nextColor[2];
    randomPiece(next[2],&nextColor[2]);
    cx=3; cy=0;
    if(collides(cur,cx,cy)) gameOver=TRUE;
}

static void initGame(){
    memset(grid,0,sizeof(grid));
    srand((unsigned)time(NULL));
    for(int i=0;i<3;i++) randomPiece(next[i],&nextColor[i]);
    spawnNext();
    score=0; bombCount=1; multCount=1;
    multiplier=1; multClearLeft=0;
    lastBombScore=0; lastMultScore=0;
    gameOver=FALSE; tickAccum=0; level=0; linesCleared=0;
}

/* ticks needed before auto-drop (decreases with level) */
static int dropInterval(){
    int d=8-level;
    return d<1?1:d;
}

/* ── moves ───────────────────────────────────────────────────────────────────── */
static void moveLeft(){
    if(!gameOver && !collides(cur,cx-1,cy)) cx--;
}
static void moveRight(){
    if(!gameOver && !collides(cur,cx+1,cy)) cx++;
}
static void softDrop(){
    if(!gameOver){
        if(!collides(cur,cx,cy+1)) cy++;
        else{ lockPiece(); checkLines(); spawnNext(); }
    }
}
static void hardDrop(){
    if(gameOver) return;
    while(!collides(cur,cx,cy+1)) cy++;
    lockPiece(); checkLines(); spawnNext();
}
static void rotatePiece(){
    if(gameOver) return;
    int tmp[4][4]={0};
    for(int r=0;r<4;r++) for(int c=0;c<4;c++)
        if(cur[r][c]) tmp[c][3-r]=cur[r][c];
    int kicks[]={0,-1,1,-2,2};
    for(int k=0;k<5;k++){
        if(!collides(tmp,cx+kicks[k],cy)){
            copyShape(cur,tmp); cx+=kicks[k]; return;
        }
    }
}
static void useBomb(){
    if(bombCount<=0||gameOver) return;
    bombCount--;
    /* clear bottom 2 rows then shift down */
    for(int c=0;c<COLS;c++){grid[ROWS-1][c]=0;grid[ROWS-2][c]=0;}
    for(int pass=0;pass<2;pass++){
        for(int r=ROWS-1;r>0;r--){
            int empty=1;
            for(int c=0;c<COLS;c++) if(grid[r][c]){empty=0;break;}
            if(empty){
                for(int rr=r;rr>0;rr--)
                    for(int c=0;c<COLS;c++) grid[rr][c]=grid[rr-1][c];
                for(int c=0;c<COLS;c++) grid[0][c]=0;
            }
        }
    }
}
static void useMult(){
    if(multCount<=0||multiplier>1||gameOver) return;
    multCount--; multiplier=2; multClearLeft=3;
}

/* ── JSON output ────────────────────────────────────────────────────────────── */
static void emitState(){
    /* ghost piece */
    int gy=cy;
    while(!collides(cur,cx,gy+1)) gy++;

    printf("{");
    printf("\"gameOver\":%s,", gameOver?"true":"false");
    printf("\"score\":%d,\"best\":%d,", score, bestScore);
    printf("\"bombs\":%d,\"mults\":%d,", bombCount, multCount);
    printf("\"multiplier\":%d,\"multLeft\":%d,", multiplier, multClearLeft);
    printf("\"level\":%d,", level);
    /* grid */
    printf("\"grid\":[");
    for(int r=0;r<ROWS;r++){
        printf("[");
        for(int c=0;c<COLS;c++)
            printf("%d%s",grid[r][c],c<COLS-1?",":"");
        printf("]%s",r<ROWS-1?",":"");
    }
    printf("],");
    /* current piece */
    printf("\"cur\":{\"x\":%d,\"y\":%d,\"color\":%d,\"shape\":[",cx,cy,curColor);
    for(int r=0;r<4;r++){
        printf("[");
        for(int c=0;c<4;c++) printf("%d%s",cur[r][c],c<3?",":"");
        printf("]%s",r<3?",":"");
    }
    printf("]},");
    /* ghost */
    printf("\"ghost\":{\"x\":%d,\"y\":%d},",cx,gy);
    /* next 3 */
    printf("\"next\":[");
    for(int n=0;n<3;n++){
        printf("{\"color\":%d,\"shape\":[",nextColor[n]);
        for(int r=0;r<4;r++){
            printf("[");
            for(int c=0;c<4;c++) printf("%d%s",next[n][r][c],c<3?",":"");
            printf("]%s",r<3?",":"");
        }
        printf("]}%s",n<2?",":"");
    }
    printf("]}\n");
    fflush(stdout);
}

/* ── tick (called by Python every ~100ms) ─────────────────────────────────── */
static void tick(){
    if(gameOver||!started) return;
    tickAccum++;
    if(tickAccum>=dropInterval()){
        tickAccum=0;
        softDrop();
    }
}

/* ── main loop ───────────────────────────────────────────────────────────────── */
int main(){
    /* line-buffer stdout so Python reads line by line */
    setvbuf(stdout,NULL,_IONBF,0);
    char line[256];
    while(fgets(line,sizeof(line),stdin)){
        /* parse cmd */
        char cmd[32]={0}, key[32]={0};
        /* simple parse: look for "cmd":"VALUE" and "key":"VALUE" */
        char *p;
        if((p=strstr(line,"\"cmd\":"))){
            sscanf(p,"\"cmd\":\"%31[^\"]\"",cmd);
        }
        if((p=strstr(line,"\"key\":"))){
            sscanf(p,"\"key\":\"%31[^\"]\"",key);
        }

        if(strcmp(cmd,"start")==0){
            initGame(); started=TRUE;
        } else if(strcmp(cmd,"tick")==0){
            tick();
        } else if(strcmp(cmd,"key")==0){
            if     (strcmp(key,"left")==0)   moveLeft();
            else if(strcmp(key,"right")==0)  moveRight();
            else if(strcmp(key,"rotate")==0) rotatePiece();
            else if(strcmp(key,"drop")==0)   hardDrop();
            else if(strcmp(key,"down")==0)   softDrop();
            else if(strcmp(key,"bomb")==0)   useBomb();
            else if(strcmp(key,"mult")==0)   useMult();
        }
        emitState();
    }
    return 0;
}
