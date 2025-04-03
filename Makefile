
all: mem_chk_perm.c
	gcc $(CFLAGS) -o mem_chk_perm mem_chk_perm.c
static: mem_chk_perm.c
	gcc $(CFLAGS) --static -o mem_chk_perm mem_chk_perm.c
clean: mem_chk_perm
	rm mem_chk_perm
