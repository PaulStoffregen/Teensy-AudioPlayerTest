
#include "player.h"
#include <Audio.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>

//#include <ESP8266.h>
#include "debug.h"

#include "mp3dec.h"

// GUItool: begin automatically generated code
AudioPlayQueue			 queue2;		 //xy=316,227
AudioPlayQueue			 queue1;		 //xy=317,187
AudioPlaySdWav			 playWav1;		 //xy=326,66
AudioMixer4				 mixer1;		 //xy=552,85
AudioMixer4				 mixer2;		 //xy=553,170
AudioOutputI2S			 i2s1;			 //xy=767,118
AudioConnection			 patchCord1(queue2, 0, mixer2, 1);
AudioConnection			 patchCord2(queue1, 0, mixer1, 1);
AudioConnection			 patchCord3(playWav1, 0, mixer1, 0);
AudioConnection			 patchCord4(playWav1, 1, mixer2, 0);
AudioConnection			 patchCord5(mixer1, 0, i2s1, 0);
AudioConnection			 patchCord6(mixer2, 0, i2s1, 1);
AudioControlSGTL5000	 sgtl5000_1;	 //xy=771,33
// GUItool: end automatically generated code

//Esp8266 wlan;

int ram;
bool sdAvailable;

#define SD_BUF_SIZE 3072 //SD-Buffer
#define MP3_BUF_SIZE (MAX_NCHAN * MAX_NGRAN * MAX_NSAMP) //MP3 output buffer

/*
void initPWR3V3(){
	pinMode(PIN_SHUTDOWNPWR3V3, OUTPUT);
	digitalWrite(PIN_SHUTDOWNPWR3V3, PWR3V3_ON);
}
*/

void setup() {
	initDBG();
	//initPWR3V3();
	ram = FreeRam();
	DBG.printf("\r\nTeensy Player\r\nF_CPU: %d MHz F_BUS: %d MHz Bytes Free: %d(%d KB)\r\n",
								F_CPU / 1000000, F_BUS / 1000000, ram, ram / 1024);

	//wlan.modeAT();

	SPI.setMOSI(PIN_SPI_MOSI);
	SPI.setSCK(PIN_SPI_SCK);

	sdAvailable = SD.begin(PIN_SPI_SDCARD_CS);
	if (!sdAvailable) {
		DBG.println("Unable to access the SD card");
	}

	// Audio connections require memory to work.
	// Should be >=18 for MP3, all buffers are you define here will be used.
	AudioMemory(20);

	sgtl5000_1.enable();
	sgtl5000_1.enhanceBassEnable();
	sgtl5000_1.volume(0.5);

}

// read from the SD card, true=ok, false=unable to read
// the SD library is much faster if all reads are 512 bytes
// this function lets us easily read any size, but always
// requests data from the SD library in 512 byte blocks.
// (c) Paul Stoffregen


//TODO : Organize this sd-stuff better...
inline bool sd_card_read(File f,void *ptr, unsigned int len)
{
	static unsigned char buffer[512];
	static unsigned int bufpos = 0;
	static unsigned int buflen = 0;
	unsigned char *dest = (unsigned char *)ptr;
	unsigned int n;

	while (len > 0) {
	if (buflen == 0) {
	  n = f.read(buffer, 512);
	  if (n == 0) return false;
	  buflen = n;
	  bufpos = 0;
	}
	unsigned int n = buflen;
	if (n > len) n = len;
	memcpy(dest, buffer + bufpos, n);
	dest += n;
	bufpos += n;
	buflen -= n;
	len -= n;
	}
	return true;
}
//todo: see above
inline uint32_t fillReadBuffer(File f, uint8_t* sd_buf, uint8_t* pData, uint32_t dataLeft)
{
	uint32_t spaceLeft;
	uint32_t read;

	// move last, small chunk from end of buffer to start, then fill with new data
	memmove(sd_buf, pData, dataLeft);
	spaceLeft = SD_BUF_SIZE - dataLeft;
	bool res = sd_card_read(f, sd_buf + dataLeft, spaceLeft);
	if (res) read = spaceLeft; else read = 0;
	if(read < spaceLeft) //fill remaining buf with 0
	{
		memset(sd_buf + dataLeft + read, spaceLeft - read, 0);
	}
	return read;
}

void idlePlay(void)
//Called whenever the decoders have time to.
//Do what you you want here, but do it fast ;-)
//You have a few ms.
{
/*
		while (ESP8266_SERIAL.available()) {
			DBG.write( ESP8266_SERIAL.read() );
		}
		while (DBG.available()) {
			ESP8266_SERIAL.write( DBG.read() );
		}
*/
}

void playWav(const char *filename)
{
	mixer1.gain(0, 1.0); //WAV
	mixer2.gain(0, 1.0);
	mixer1.gain(1, 0.0);
	mixer2.gain(1, 0.0);

	playWav1.play(filename);
	delay(5);
	AudioProcessorUsageMaxReset();
	AudioMemoryUsageMaxReset();
	while ( playWav1.isPlaying() )
	{
		idlePlay();
	}
	DBG.print("AudioProcessorUsageMax: ");
	DBG.print( AudioProcessorUsageMax() );
	DBG.print("% AudioMemoryUsageMax: ");
	DBG.println( AudioMemoryUsageMax() );
}

void playMp3(const char *filename)
{
	File			file;
	uint8_t			sd_buf[SD_BUF_SIZE];
	uint8_t			*sd_p;
	int				sd_left;
	bool			sd_eof;

	uint16_t		buf[MP3_BUF_SIZE];

	int				decode_res;
	uint32_t		frames_decoded;

	HMP3Decoder		hMP3Decoder;
	MP3FrameInfo	mp3FrameInfo;

	int	max_time=0;
	int	max_time_sd=0;
	int	max_time_decode=0;
	int	max_time_audiobuffers=0;

	mixer1.gain(0, 0.0); //WAV
	mixer2.gain(0, 0.0);
	mixer1.gain(1, 1.0);
	mixer2.gain(1, 1.0);

	file = SD.open(filename);
	if (!file) {
		Serial2.print("Could not open ");
		Serial2.println(filename);
		return;
	}

	hMP3Decoder = MP3InitDecoder();
	if(hMP3Decoder == NULL)
	{
		Serial2.println("Failed to init mp3.");
		file.close();
		return;
	}

	AudioProcessorUsageMaxReset();
	AudioMemoryUsageMaxReset();

	DBG.printf("Bytes Free: %d \r\n", FreeRam());

	decode_res = ERR_MP3_NONE;
	frames_decoded = 0;

	sd_eof = false;
	sd_p = sd_buf;
	sd_left = 0;

	do
	{

		int t = micros();
		int t1 = t;
		if (sd_left < (2 * MAINBUF_SIZE) && !sd_eof) {
			uint32_t read = fillReadBuffer(file, sd_buf, sd_p, sd_left);
			sd_left += read;
			sd_p = sd_buf;
		}
		t1 = micros() - t;
		if (t1>max_time_sd) max_time_sd = t1;

		// find start of next MP3 frame - assume EOF if no sync found
		int offset = MP3FindSyncWord(sd_p, sd_left);
		if (offset < 0) {
			DBG.println("Mp3Decode: No sync");
			sd_eof = true;
			//goto stop;
		}

		sd_p += offset;
		sd_left -= offset;

		// decode one MP3 frame - if offset < 0 then sd_left was less than a full frame
		int t2 = micros();
		decode_res = MP3Decode(hMP3Decoder, &sd_p, &sd_left,(short*)buf, 0);
		t2 = micros()-t;
		if (t2>max_time_decode) max_time_decode=t2;

		switch (decode_res)
		{
			case ERR_MP3_INDATA_UNDERFLOW:
				{
					DBG.println("Mp3Decode: Decoding error ERR_MP3_INDATA_UNDERFLOW");
					sd_eof = true;
					break;
				}
			case ERR_MP3_FREE_BITRATE_SYNC:
				{
					break;
				}
			case ERR_MP3_MAINDATA_UNDERFLOW:
				{
					DBG.print("Mp3Decode: Maindata underflow\r\n");
					// do nothing - next call to decode will provide more mainData
					break;
				}
			case ERR_MP3_NONE:
				{
					MP3GetLastFrameInfo(hMP3Decoder, &mp3FrameInfo);

				int t1 = micros() - t;
				if (t1 > max_time) max_time = t1;

				if (frames_decoded == 0)
				{
					DBG.printf("Mp3Decode: %d Hz %d Bit %d Channels\r\n", mp3FrameInfo.samprate, mp3FrameInfo.bitsPerSample, mp3FrameInfo.nChans);
					if((mp3FrameInfo.samprate != 44100) || (mp3FrameInfo.bitsPerSample != 16) || (mp3FrameInfo.nChans != 2)) {
						Serial2.println("Mp3Decode: incompatible MP3 file.");
					}
				}

				//debug only..
				if((frames_decoded) % 100 == 0)
				{
					DBG.printf("Mp3Decode: frame %u, bitrate=%d, max time=%d max time audiobuffers=%d max time sd=%d\r\n", frames_decoded, mp3FrameInfo.bitrate, max_time, max_time_audiobuffers, max_time_sd);
					max_time_sd=0;
					max_time_decode=0;
					max_time_audiobuffers=0;
					max_time =0;
				}

				frames_decoded++;

				int len = mp3FrameInfo.outputSamps;	//Should be 2304.

				int j = 0;
				int16_t *lb;
				int16_t *rb;

				int t3=0;

				//The Sketch will be most of the time here:
				while (len) {
					while (!queue2.available()) idlePlay();
					lb = queue1.getBuffer();
					while (!queue2.available()) idlePlay();

					rb = queue2.getBuffer();
					t3 = micros();

					//deinterlace audiodata. lrlrlrlr -> llll rrrr
					//
					//unrolled loop is 30% faster: TODO: read 32 Bit at once ??
					for (int i=0; i < AUDIO_BLOCK_SAMPLES / 4; i++){

						*lb++ = buf[j++];
						*rb++ = buf[j++];

						*lb++ = buf[j++];
						*rb++ = buf[j++];

						*lb++ = buf[j++];
						*rb++ = buf[j++];

						*lb++ = buf[j++];
						*rb++ = buf[j++];

					}

					t3 = micros() - t3;
					queue1.playBuffer();
					queue2.playBuffer();
					len -= AUDIO_BLOCK_SAMPLES * 2;
					//Todo: length of last frame ?
				}

				if (t3 > max_time_audiobuffers) max_time_audiobuffers = t3;

				break;
			}

			default :
			{
				DBG.println("Mp3Decode: Decoding error");
				sd_eof = true;
				break;
			}
		}

	} while(!sd_eof);

	file.close();
	MP3FreeDecoder(hMP3Decoder);

	DBG.printf("Max Time: sd:%d us, decode: %d ms, audiobuffers: %d us,	 sum: %d ms\r\n",max_time_sd , max_time_decode / 1000, max_time_audiobuffers, max_time / 1000 );
	DBG.print("AudioProcessorUsageMax: ");
	DBG.print( AudioProcessorUsageMax() );
	DBG.print("% AudioMemoryUsageMax: ");
	DBG.println( AudioMemoryUsageMax() );
}

void playFile(const char *filename)
{

	DBG.printf("Playing file: %s\r\n",filename);
	if (!sdAvailable) {
		DBG.println("ERROR: SD not available!");
		return;
	}

	char *c = strrchr(filename, '.');

	if ( (strcmp(c, ".wav")==0) || (stricmp(c, ".WAV")==0)	)
	{
		playWav(filename);
	}
	else if ( (strcmp(c, ".mp3")==0) || (strcmp(c, ".MP3")==0)	)
	{
		playMp3(filename);
	}

}

void loop() {
	playFile("whisper.mp3");
	delay(500);
	playFile("Song1.mp3");
	delay(500);
	playFile("Dream.mp3");
	delay(500);
	playFile("SDTEST1.WAV");
	delay(500);
	playFile("SDTEST2.WAV");
	delay(500);
	playFile("SDTEST3.WAV");
	delay(500);
	playFile("SDTEST4.WAV");
	delay(1500);
}


