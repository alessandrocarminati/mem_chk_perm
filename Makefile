all: mem_chk_perm.c
	gcc -o mem_chk_perm mem_chk_perm.c
clean: mem_chk_perm
	rm mem_chk_perm
