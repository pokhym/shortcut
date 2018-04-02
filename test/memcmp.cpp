#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include <map>
#include <string>
using namespace std;

static char* map_file (const char* filename, u_long& size) 
{
    struct stat st;

    int fd = open (filename, O_RDONLY);
    if (fd < 0) {
	fprintf (stderr, "Cannot open %s\n", filename);
	return NULL;
    }

    int rc = fstat (fd, &st);
    if (rc < 0) {
	fprintf (stderr, "Cannot stat %s\n", filename);
	return NULL;
    }
    size = st.st_size;
    
    char* p = (char *) mmap (0, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
	fprintf (stderr, "Cannot map %s size %ld\n", filename, size);
	return NULL;
    }

    close(fd);
    
    return p;
}

static int compare_files (const char* filename1, const char* filename2, long offset) 
{
    u_long size1, size2, addr = 0;
    int bytes_diff = 0;

    for (int i = strlen(filename1); i > 0; i--) {
	if (filename1[i] == '.') {
	    addr = strtoul(filename1+i+1, 0, 16);
	    break;
	}
    }
    if (addr == 0) {
	fprintf (stderr, "bad file name: %s\n", filename1);
	return -1;
    }

    char* region1 = map_file (filename1, size1);
    char* region2 = map_file (filename2, size2);
    if (region1 == NULL || region2 == NULL) return -1;
    
    if (size1+offset != size2) {
	fprintf (stderr, "file %s has size %ld but file %s has size %ld\n", filename1, size1, filename2, size2);
	return -1;
    }

    if (offset > 0) {
	printf ("skip first 0x%lx bytes of %s\n", offset, filename2);
	for (u_long i = 0; i < size1; i++) {
	    if (region1[i] != region2[i+offset]) {
		printf ("%s vs. %s: byte 0x%08lx (%08lx) different: 0x%02x vs. 0x%02x\n", filename1, filename2, addr+i, i, region1[i]&0xff, region2[i]&0xff);
		bytes_diff++;
	    }
	}
    } else if (offset < 0){
	printf ("skip first 0x%lx bytes of %s\n", -offset, filename1);
	for (u_long i = 0; i < size1+offset; i++) {
	    if (region1[i-offset] != region2[i]) {
		printf ("%s vs. %s: byte 0x%08lx (%08lx) different: 0x%02x vs. 0x%02x\n", filename1, filename2, addr+i, i, region1[i]&0xff, region2[i]&0xff);
		bytes_diff++;
	    }
	}
    } else {
	for (u_long i = 0; i < size1; i++) {
	    if (region1[i] != region2[i]) {
		printf ("%s vs. %s: byte 0x%08lx (%08lx) different: 0x%02x vs. 0x%02x\n", filename1, filename2, addr+i, i, region1[i]&0xff, region2[i]&0xff);
		bytes_diff++;
	    }
	}
    }

    munmap (region1, size1);
    munmap (region2, size2);

    return 0;
}

int main (int argc, char* argv[])
{
    map<u_long, string> slice_regions;
    map<u_long, string> replay_regions;

    DIR* dirp = opendir("/tmp");
    if (dirp == NULL) {
	fprintf (stderr, "Cannot open /tmp directory\n");
	return -1;
    }

    struct dirent* dp;
    while ((dp = readdir (dirp)) != NULL) {
	if (!strncmp (dp->d_name, "slice_vma.", 10)) {
	    string filename = "/tmp/" + string(dp->d_name);
	    u_long addr = strtoul(dp->d_name+10, 0, 16);
	    slice_regions[addr] = filename;
	}
    }

    closedir (dirp);

    dirp = opendir("/replay_logdb/rec_147503/last_altex/");
    if (dirp == NULL) {
	fprintf (stderr, "Cannot open replay ckpt directory\n");
	return -1;
    }
    while ((dp = readdir (dirp)) != NULL) {
	if (!strncmp (dp->d_name, "ckpt.78193.ckpt_mmap.", 21)) {
	    string filename = "/replay_logdb/rec_147503/last_altex/" + string(dp->d_name);
	    u_long addr = strtoul(dp->d_name+21, 0, 16);
	    replay_regions[addr] = filename;
	}
    }
    closedir (dirp);

    for (map<u_long, string>::iterator it = slice_regions.begin(); it != slice_regions.end(); it++) {
	map<u_long, string>::iterator it2 = replay_regions.find(it->first);
	if (it2 != replay_regions.end()) {
	    compare_files (it->second.c_str(), it2->second.c_str(), 0);
	}
    }

    u_long last1 = 0;
    string last_filename1;
    for (map<u_long, string>::iterator it = slice_regions.begin(); it != slice_regions.end(); it++) {
	if (it->first > last1) {
	    last1 = it->first;
	    last_filename1 = it->second;
	}
	map<u_long, string>::iterator it2 = replay_regions.find(it->first);
	if (it2 == replay_regions.end()) {
	    printf ("Region %lx is in slice regions but not replay regions\n", it->first);
	} else {
	    last1 = 0;
	}
    }

    u_long last2 = 0;
    string last_filename2;
    for (map<u_long, string>::iterator it = replay_regions.begin(); it != replay_regions.end(); it++) {
	if (it->first > last2) {
	    last2 = it->first;
	    last_filename2 = it->second;
	}
	map<u_long, string>::iterator it2 = slice_regions.find(it->first);
	if (it2 == slice_regions.end()) {
	    printf ("Region %lx is in replay regions but not slice regions\n", it->first);
	} else {
	    last2 = 0;
	}
    }
    if (last1 && last2) {
	printf ("Comparing uneven stack regions\n");
	compare_files (last_filename1.c_str(), last_filename2.c_str(), last1-last2);
    }
    
    return 0;
}
