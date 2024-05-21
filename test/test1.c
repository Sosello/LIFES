#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/ioctl.h>

#include "command.h"

#define FILE_SIZE 1024
#define DATA_SIZE 256

int copy_file(int fd, char *path) 
{
    clock_t start, end;
    lseek(fd, 0, SEEK_SET);
    int file = open(path, O_CREAT | O_WRONLY, 0600);
    if (file < 0) {
        perror("Error opening file");
        return 1;
    }
    start = clock();
    char data[DATA_SIZE];
    int read_bytes;
    while ((read_bytes = read(fd, data, sizeof(data))) > 0) {
        write(file, data, read_bytes);
    }
    end = clock();
    close(file);
    printf("Copy time: %f\n", ((double) (end - start)) / CLOCKS_PER_SEC);
    return 0;
}


int insert_file(char* message, char *path, int offset) 
{   
    struct stat st;
    if (stat(path, &st) < 0) {
        perror("Error getting file size");
        return 1;
    }
    printf("File size: %ld\n", st.st_size);
    if ((off_t)offset > st.st_size) {
        perror("Offset is greater than file size");
        return 1;
    }
    clock_t start, end;
    int file_w = open(path, O_RDWR, 0600);
    if ((file_w) < 0) {
        perror("Error opening file");
        return 1;
    }
    start = clock();
    char *buffer_message = malloc(st.st_size - offset);
    if (!buffer_message) {
        perror("Error allocating memory");
        return 1;
    }
    lseek(file_w, offset, SEEK_SET);
    read(file_w, buffer_message, st.st_size - offset);
    lseek(file_w, offset, SEEK_SET);
    if(write(file_w, message, strlen(message)) < 0) {
        perror("Error writing file");
        return 1;
    }
    if(write(file_w, buffer_message, st.st_size - offset) < 0) {
        perror("Error writing file");
        return 1;
    }
    end = clock();
    close(file_w);
    free(buffer_message);
    return 0;
}

int main() 
{
    int file = open("./test.txt", O_RDONLY);
    if (file < 0) {
        perror("Error opening file");
        return 1;
    }
    copy_file(file, "/mnt/ouichefs/file1");
    insert_file("Hello World", "/mnt/ouichefs/file1", 5);
    copy_file(file, "/mnt/ouichefs/file2");

    int file1  = open("/mnt/ouichefs/file1", O_RDONLY);
    if (file1 < 0) {
        perror("Error opening file");
        return 1;
    }


    // ioctl tests
    char buf[100]="";
    if(ioctl(file1, USED_BLOCKS, buf)==-1)
        perror("Failed to system call ioctl in USED_BLOCKS request\n");
    printf("resultat of ioctl USED_BLOCKS: %s\n", buf);
    if(ioctl(file1, PARTIALLY_BLOCKS, buf)==-1)
        perror("Failed to system call ioctl in FREE_BLOCKS request\n");
    printf("resultat of ioctl PARTIALLY_BLOCKS: %s\n", buf);
    if(ioctl(file1, WASTED_BYTES, buf)==-1)
        perror("Failed to system call ioctl in FREE_BLOCKS request\n");
    printf("resultat of ioctl WASTED_BYTES: %s\n", buf);
    if(ioctl(file1, LIST_USED_BLOCKS, buf)==-1)
        perror("Failed to system call ioctl in LIST_USED request\n");
    printf("resultat of ioctl WASTED_BYTES: %s\n", buf);
    
    copy_file(file1, "/mnt/ouichefs/file3");
    close(file);
    close(file1);
    return 0;
}