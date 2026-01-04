all: picturephone

picturephone: picturephone.c
	gcc -o picturephone picturephone.c -Wall -W -O2

clean:
	rm picturephone
