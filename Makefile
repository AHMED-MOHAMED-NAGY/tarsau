hepsi: derle calistir

derle:
	gcc -o ./lib/tarsau.o -c ./src/tarsau.c
	gcc -o ./bin/tarsau ./lib/tarsau.o

calistir:
	./bin/tarsau