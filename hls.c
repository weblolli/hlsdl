#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>
#include "curl.h"
#include "hls.h"

static char *get_rndstring(int length)
{
    int max;
    const char *letters = "abcdefghijklmnopqrstuvwxyz123456789";
    int i, r;
    srand((int)time(NULL));
    max = length;
    
    char *generated = (char*)malloc(max + 1);
    for(i = 0; i < max; i++) {
        r = rand() % strlen(letters);
        generated[i] = letters[r];
    }
    generated[max] = '\0';
    return generated;
}

int get_playlist_type(char *source)
{
    if (strncmp("#EXTM3U", source, 7) != 0) {
        fprintf(stderr, "Not a valid M3U8 file. Exiting.\n");
        return -1;
    }
    for (int i = 0; i < strlen(source); i++) {
        if(!strncmp(&source[i], "#EXT-X-STREAM-INF", 17)) {
            return 0;
        }
    }
    return 1;
}

static int extend_url(char **url, const char *baseurl)
{
    size_t max_length = strlen(*url) + strlen(baseurl) + 10;
    
    if (!strncmp(*url, "http://", 7) || !strncmp(*url, "https://", 8)) {
        return 0; //absolute url, nothing to do
    }
    
    else if (**url == '/') {
        char *domain = (char*)malloc(max_length);
        strcpy(domain, baseurl);
        if(!sscanf(baseurl, "http://%[^/]", domain)) {
            sscanf(baseurl, "https://%[^/]", domain);
        }
        
        char *buffer = (char*)malloc(max_length);
        snprintf(buffer, max_length, "%s%s", domain, *url);
        strcpy(*url, buffer);
        free(buffer);
        free(domain);
        return 0;
    }
    
    else {
        char *buffer = (char*)malloc(max_length);
        snprintf(buffer, max_length, "%s/../%s", baseurl, *url);
        strcpy(*url, buffer);
        free(buffer);
        return 0;
    }

}

static int parse_playlist_tag(struct hls_media_playlist *me, char *tag)
{
    if (!strncmp(tag, "#EXT-X-KEY:METHOD=AES-128", 25)) {
        me->encryption = true;
        me->encryptiontype = ENC_AES128;
        me->enc_aes.iv_is_static = false;
        
        char *link_to_key = (char*)malloc(strlen(tag) + strlen(me->url) + 10);
        char iv_key[33];
        
        if(sscanf(tag, "#EXT-X-KEY:METHOD=AES-128,URI=\"%[^\"]\",IV=0x%s", link_to_key, iv_key) == 2) {
            strcpy(me->enc_aes.iv_value, iv_key);
            me->enc_aes.iv_is_static = true;
        }
        
        extend_url(&link_to_key, me->url);
        free(link_to_key);
        
        char *content_of_key_file = malloc(33);
        printf("%s\n", link_to_key);
        get_source_from_url(link_to_key, &content_of_key_file);
        
        char decrypt[33];
        int length = 0;
        for(int i = 0; i < strlen(content_of_key_file); i++) {
            length += sprintf(decrypt+length, "%02x", (unsigned char)content_of_key_file[i]);
        }
        strcpy(me->enc_aes.key_value, decrypt);
        free(content_of_key_file);
    }
    return 0;
}

static int get_link_count(char *source)
{
    int linkcount = 0;
    if(source) {
        for (int i = 0; i < strlen(source); i++){
            if (source[i] == 10    &&
                source[i+1] != '#' &&
                source[i+1] != 10  &&
                source[i+1] != '\0') {
                linkcount++;
            }
        }
    }
    return linkcount;
}

static int media_playlist_get_media_sequence(char *source)
{
    int j = 0;
    
    char *p_media_sequence = strstr(source, "#EXT-X-MEDIA-SEQUENCE:");
    if(sscanf(p_media_sequence, "#EXT-X-MEDIA-SEQUENCE:%d", &j) != 1) {
        fprintf(stderr, "Error reading EXT-X-MEDIA-SEQUENCE\n");
        return 0;
    }
    return j;
}

static int media_playlist_get_links(struct hls_media_playlist *me)
{
    int j = 0;
    int len = 0;
    
    int media_squence_start_val = media_playlist_get_media_sequence(me->source);
    
    struct hls_media_segment *ms = me->media_segment;
    
    /* Initialze the Strings */
    for(int i = 0; i < me->count; i++) {
        ms[i].url = (char*)malloc(strlen(me->source));
    }
    
    /* Get Individual Links */
    for (int i = 0; i < strlen(me->source); i++) {
        /* Skip this line if there is a # at the beginning */
        if(me->source[i] == '#') {
            parse_playlist_tag(me, &me->source[i]);
            while(me->source[i] != 10){
                i++;
            }
            continue;
        }
        
        else if (me->source[i - 1] == 10 && me->source[i] == 10) { //skip blank line
            continue;
        }
        
        /* Write the links in a char array */
        else {
            if(me->source[i] != 10) {
                len += snprintf(ms[j].url + len, strlen(me->source), "%c", me->source[i]);
            }
            else if (me->source[i] == '\0') {
                break;
            }
            else {
                if (j == me->count) {
                    fprintf(stderr, "Link overflow");
                    return 1; //ERROR
                }
                ms[j].sequence_number = j + media_squence_start_val;
                if (me->encryptiontype == ENC_AES128) {
                    strcpy(ms[j].enc_aes.key_value, me->enc_aes.key_value);
                    if (me->enc_aes.iv_is_static == false) {
                        snprintf(ms[j].enc_aes.iv_value, 33, "%032x\n", ms[j].sequence_number);
                    }
                }
                j++;
                len = 0;
            }
        }
    }
    /* Extend individual urls */
    for (int i = 0; i < me->count; i++) {
        extend_url(&ms[i].url, me->url);
    }
    return 0;
}

int handle_hls_media_playlist(struct hls_media_playlist *me)
{
    get_source_from_url(me->url, &me->source);
    if (get_playlist_type(me->source) != MEDIA_PLAYLIST) {
        return 1;
    }
    me->count = get_link_count(me->source);
    me->media_segment = (struct hls_media_segment*)malloc(sizeof(struct hls_media_segment) * me->count);

    if (media_playlist_get_links(me)) {
        return 1;
    }
    return 0;
}

static int master_playlist_get_bitrate(struct hls_master_playlist *ma)
{
    int j = 0;
    
    struct hls_media_playlist *me = ma->media_playlist;
    /* Get Individual Bitrate */
    for (int i = 0; i < strlen(ma->source); i++) {
        /* Check for BANDWIDTH= */
        if(!strncmp(&ma->source[i], "BANDWIDTH=", 10) && j < ma->count) {
            char *ascii_number = (char*)malloc(11); //length of an uint + 1
            
            if(sscanf(&ma->source[i], "BANDWIDTH=%[0-9]", ascii_number) != 1) {
                fprintf(stderr, "Error finding bitrate\n");
                return 1;
            }
            me[j].bitrate = atoi(ascii_number);
            j++;
            free(ascii_number);
        }
    }
    
    if (j < ma->count) {
        /* Set undefined bitrates to zero */
        for (int i = j; i < ma->count; i++) {
            me[i].bitrate = 0;
        }
    }
    return 0;
}

static int master_playlist_get_links(struct hls_master_playlist *ma)
{
    
    int j = 0;
    int len = 0;
    
    struct hls_media_playlist *me = ma->media_playlist;

    /* Initialze the Strings */
    for(int i = 0; i < ma->count; i++) {
        if((me[i].url = (char*)malloc(strlen(ma->source))) == NULL) {
            fprintf(stderr, "OUT OF MEMORY\n");
            return 1;
        }
    }
    
    /* Get Individual Qualitys */
    for (int i = 0; i < strlen(ma->source); i++) {
        /* Skip this line if there is a # at the beginning */
        if(ma->source[i] == '#') {
            while(ma->source[i] != 10){
                i++;
            }
            continue;
        }
        
        else if (ma->source[i - 1] == 10 && ma->source[i] == 10) { //skip blank line
            continue;
        }
        
        /* Write the links in a char array */
        else {
            if(ma->source[i] != 10) {
                len += snprintf(me[j].url + len, strlen(ma->source), "%c", ma->source[i]);
            }
            else if (ma->source[i] == '\0') {
                break;
            }
            else {
                if (j == ma->count) {
                    fprintf(stderr, "Link overflow");
                    return 1; //ERROR
                }
                
                j++;
                len = 0;
            }
        }
    }
    /* Extend individual urls */
    for (int i = 0; i < ma->count; i++) {
        extend_url(&me[i].url, ma->url);
    }
    return 0;
}

int handle_hls_master_playlist(struct hls_master_playlist *ma)
{
    ma->count = get_link_count(ma->source);
    ma->media_playlist = (struct hls_media_playlist*)malloc(sizeof(struct hls_media_playlist) * ma->count);
    if (master_playlist_get_links(ma)) {
        return 1;
    }
    if (master_playlist_get_bitrate(ma)) {
        return 1;
    }
    return 0;
}

void print_hls_master_playlist(struct hls_master_playlist *ma)
{
    int i;
    printf("Found %d Qualitys\n\n", ma->count);
    for(i = 0; i < ma->count; i++) {
        printf("%d: Bandwidth: %d, Link: %s\n", i, ma->media_playlist[i].bitrate, ma->media_playlist[i].url);
    }
}

int download_hls(struct hls_media_playlist *me)
{
    FILE *txtfile;
    char foldername[20];
    char txtfilepath[30];
    char systemcall[100];
    char *rndstring = get_rndstring(10);
    sprintf(foldername, "tmp_%s", rndstring);
    sprintf(systemcall, "mkdir %s", foldername);
    sprintf(txtfilepath, "%s/list.txt", foldername);
    
    system(systemcall); //make folder
    
    txtfile = fopen(txtfilepath, "w");
    
    for (int i = 0; i < me->count; i++) {
        char name[30];
        sprintf(name, "%s/%d.ts", foldername, i);
        dl_file(me->media_segment[i].url, name);
        fprintf(txtfile, "file \'%d.ts\'\n", i);
        if (me->encryption == true) {
            if (me->encryptiontype == ENC_AES128) {
                char opensslcall[300];
                snprintf(opensslcall, 300, "openssl aes-128-cbc -d -in %s -out %s/tmp_file -K %s -iv %s ; mv %s/tmp_file %s",
                         name, foldername, me->media_segment[i].enc_aes.key_value, me->media_segment[i].enc_aes.iv_value, foldername, name);
                system(opensslcall);
            }
        }
    }
    fclose(txtfile);
    
    sprintf(systemcall, "ffmpeg -loglevel quiet -f concat -i %s -c copy out.ts", txtfilepath);
    
    system(systemcall); //ffmpeg cmnd
    
    sprintf(systemcall, "rm -rf %s/*.ts", foldername);
    system(systemcall); //rm ts files
    sprintf(systemcall, "rm -rf %s/list.txt", foldername);
    system(systemcall); //rm list
    sprintf(systemcall, "rm -r %s/", foldername);
    system(systemcall); //rm folder
    free(rndstring);
    return 0;
}

void media_playlist_cleanup(struct hls_media_playlist *me)
{
    free(me->source);
    free(me->url);
    for (int i = 0; i < me->count; i++) {
        free(me->media_segment[i].url);
    }
    free(me->media_segment);
}

void master_playlist_cleanup(struct hls_master_playlist *ma)
{
    free(ma->source);
    free(ma->url);
    free(ma->media_playlist);
}
