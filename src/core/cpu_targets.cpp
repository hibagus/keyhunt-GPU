/* Target-loading code extracted from AlbertoBSD's keyhunt CPU implementation. */
#include "keyhunt/core/cpu_targets.h"
#include "keyhunt/core/util.h"
#include "keyhunt/crypto/secp256k1/SECP256k1.h"
#include "keyhunt/crypto/hash/sha256.h"
#include "base58/libbase58.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>

namespace keyhunt::core {

CpuTargetTable::~CpuTargetTable() {
    free(entries);
    free(filter.bf);
}

void checkpointer(void *ptr,const char *file,const char *function,const  char *name,int line)	{
	if(ptr == NULL)	{
		fprintf(stderr,"[E] error in file %s, %s pointer %s on line %i\n",file,function,name,line);
		exit(EXIT_FAILURE);
	}
}

bool isBase58(char c) {
    // Define the base58 set
    const char base58Set[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    // Check if the character is in the base58 set
    return strchr(base58Set, c) != NULL;
}

bool isValidBase58String(char *str)	{
	int len = strlen(str);
	bool continuar = true;
	for (int i = 0; i < len && continuar; i++) {
		continuar = isBase58(str[i]);
	}
	return continuar;
}

bool initBloomFilter(struct bloom *bloom_arg,uint64_t items_bloom, int multiplier)	{
	bool r = true;
	printf("[+] Bloom filter for %" PRIu64 " elements.\n",items_bloom);
	if(items_bloom <= 10000)	{
		if(bloom_init2(bloom_arg,10000,0.000001) == 1){
			fprintf(stderr,"[E] error bloom_init for 10000 elements.\n");
			r = false;
		}
	}
	else	{
		if(bloom_init2(bloom_arg,multiplier*items_bloom,0.000001)	== 1){
			fprintf(stderr,"[E] error bloom_init for %" PRIu64 " elements.\n",items_bloom);
			r = false;
		}
	}
	printf("[+] Loading data to the bloomfilter total: %.2f MB\n",(double)(((double) bloom_arg->bytes)/(double)1048576));
	return r;
}

bool CpuTargetTable::readFileAddress(const char *fileName)	{
	FILE *fileDescriptor;
	char fileBloomName[30];	/* Actually it is Bloom and Table but just to keep the variable name short*/
	uint8_t checksum[32],hexPrefix[9];
	char dataChecksum[32],bloomChecksum[32];
	size_t bytesRead;
	uint64_t dataSize;
	/*
		if the FLAGSAVEREADFILE is Set to 1 we need to the checksum and check if we have that information already saved
	*/
	if(config.cache_targets)	{	/* if the flag is set to REAd and SAVE the file firs we need to check it the file exist*/
		if(!sha256_file((const char*)fileName,checksum)){
			fprintf(stderr,"[E] sha256_file error line %i\n",__LINE__ - 1);
			return false;
		}
		tohex_dst((char*)checksum,4,(char*)hexPrefix); // we save the prefix (last fourt bytes) hexadecimal value
		snprintf(fileBloomName,30,"data_%s.dat",hexPrefix);
		fileDescriptor = fopen(fileBloomName,"rb");
		if(fileDescriptor != NULL)	{
			printf("[+] Reading file %s\n",fileBloomName);

			//read filter checksum (expected value to be checked)
			//read bloom filter structure
			//read bloom filter data
			//calculate checksum of the current readed data
			//Compare checksums
			//read data checksum (expected value to be checked)
			//read data size
			//read data
			//compare the expected datachecksum againts the current data checksum
			//compare the expected filter checksum againts the current filter checksum


			//read filter checksum (expected value to be checked)
			bytesRead = fread(bloomChecksum,1,32,fileDescriptor);
			if(bytesRead != 32)	{
				fprintf(stderr,"[E] Errore reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}

			//read bloom filter structure
			bytesRead = fread(&filter,1,sizeof(struct bloom),fileDescriptor);
			filter.bf = NULL; // Never own a pointer loaded from a file.
			if(bytesRead != sizeof(struct bloom))	{
				fprintf(stderr,"[E] Error reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}

			printf("[+] Bloom filter for %" PRIu64 " elements.\n",filter.entries);

			filter.bf = (uint8_t*) malloc(filter.bytes);
			if(filter.bf == NULL)	{
				fprintf(stderr,"[E] Error allocating memory, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}

			//read bloom filter data
			bytesRead = fread(filter.bf,1,filter.bytes,fileDescriptor);
			if(bytesRead != filter.bytes)	{
				fprintf(stderr,"[E] Error reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}
			if(config.skip_checksum == 0){

				//calculate checksum of the current readed data
				sha256((uint8_t*)filter.bf,filter.bytes,(uint8_t*)checksum);

				//Compare checksums
				/*
				if(FLAGDEBUG)	{
					hextemp = tohex((char*)checksum,32);
					printf("[D] Current Bloom checksum %s\n",hextemp);
					free(hextemp);
				}
				*/
				if(memcmp(checksum,bloomChecksum,32) != 0)	{
					fprintf(stderr,"[E] Error checksum mismatch, code line %i\n",__LINE__ - 2);
					fclose(fileDescriptor);
					return false;
				}
			}

			/*
			if(FLAGDEBUG) {
				hextemp = tohex((char*)filter.bf,32);
				printf("[D] first 32 bytes of the filter : %s\n",hextemp);
				bloom_print(&filter);
				printf("[D] filter.bf points to %p\n",filter.bf);
			}
			*/

			bytesRead = fread(dataChecksum,1,32,fileDescriptor);
			if(bytesRead != 32)	{
				fprintf(stderr,"[E] Errore reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}

			bytesRead = fread(&dataSize,1,sizeof(uint64_t),fileDescriptor);
			if(bytesRead != sizeof(uint64_t))	{
				fprintf(stderr,"[E] Errore reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}
			count = dataSize / sizeof(struct address_value);

			printf("[+] Allocating memory for %" PRIu64 " elements: %.2f MB\n",count,(double)(((double) sizeof(struct address_value)*count)/(double)1048576));

			entries = (struct address_value*) malloc(dataSize);
			if(entries == NULL)	{
				fprintf(stderr,"[E] Error allocating memory, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}

			bytesRead = fread(entries,1,dataSize,fileDescriptor);
			if(bytesRead != dataSize)	{
				fprintf(stderr,"[E] Error reading file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				return false;
			}
			if(config.skip_checksum == 0)	{

				sha256((uint8_t*)entries,dataSize,(uint8_t*)checksum);
				if(memcmp(checksum,dataChecksum,32) != 0)	{
					fprintf(stderr,"[E] Error checksum mismatch, code line %i\n",__LINE__ - 2);
					fclose(fileDescriptor);
					return false;
				}
			}
			//printf("[D] filter.bf points to %p\n",filter.bf);
			cache_loaded = 1;	/* We mark the file as readed*/
			fclose(fileDescriptor);
			match_bytes = sizeof(struct address_value);
		}
	}
	if(!cache_loaded)	{
		/*
			if the data_ file doesn't exist we need read it first:
		*/
		switch(config.mode)	{
			case MODE_ADDRESS:
				if(config.crypto == CRYPTO_BTC)	{
					return forceReadFileAddress(fileName);
				}
				if(config.crypto == CRYPTO_ETH)	{
					return forceReadFileAddressEth(fileName);
				}
			break;
			case MODE_MINIKEYS:
			case MODE_RMD160:
				return forceReadFileAddress(fileName);
			break;
			case MODE_XPOINT:
				return forceReadFileXPoint(fileName);
			break;
			default:
				return false;
			break;
		}
	}
	return true;
}

bool CpuTargetTable::forceReadFileAddress(const char *fileName)	{
	/* Here we read the original file as usual */
	FILE *fileDescriptor;
	bool validAddress;
	uint64_t numberItems,i;
	size_t r,raw_value_length;
	uint8_t rawvalue[50];
	char aux[100],*hextemp;
	fileDescriptor = fopen(fileName,"r");
	if(fileDescriptor == NULL)	{
		fprintf(stderr,"[E] Error opening the file %s, line %i\n",fileName,__LINE__ - 2);
		return false;
	}

	/*Count lines in the file*/
	numberItems = 0;
	while(!feof(fileDescriptor))	{
		hextemp = fgets(aux,100,fileDescriptor);
		trim(aux," \t\n\r");
		if(hextemp == aux)	{
			r = strlen(aux);
			if(r > 20)	{
				numberItems++;
			}
		}
	}
	fseek(fileDescriptor,0,SEEK_SET);
	match_bytes = 20;		/*20 bytes beacuase we only need the data in binary*/

	printf("[+] Allocating memory for %" PRIu64 " elements: %.2f MB\n",numberItems,(double)(((double) sizeof(struct address_value)*numberItems)/(double)1048576));
	entries = (struct address_value*) malloc(sizeof(struct address_value)*numberItems);
	checkpointer((void *)entries,__FILE__,"malloc","addressTable" ,__LINE__ -1 );

	if(!initBloomFilter(&filter,numberItems,config.bloom_multiplier)) {
		fclose(fileDescriptor);
		return false;
	}

	i = 0;
	while(i < numberItems)	{
		validAddress = false;
		memset(aux,0,100);
		memset(entries[i].value,0,sizeof(struct address_value));
		hextemp = fgets(aux,100,fileDescriptor);
		trim(aux," \t\n\r");
		r = strlen(aux);
		if(r > 0 && r <= 40)	{
			if(r<40 && isValidBase58String(aux))	{	//Address
				raw_value_length = 25;
				b58tobin(rawvalue,&raw_value_length,aux,r);
				if(raw_value_length == 25)	{
					//hextemp = tohex((char*)rawvalue+1,20);
					bloom_add(&filter, rawvalue+1 ,sizeof(struct address_value));
					memcpy(entries[i].value,rawvalue+1,sizeof(struct address_value));
					i++;
					validAddress = true;
				}
			}
			if(r == 40 && isValidHex(aux))	{	//RMD
				hexs2bin(aux,rawvalue);
				bloom_add(&filter, rawvalue ,sizeof(struct address_value));
				memcpy(entries[i].value,rawvalue,sizeof(struct address_value));
				i++;
				validAddress = true;
			}
		}
		if(!validAddress)	{
			fprintf(stderr,"[I] Ommiting invalid line %s\n",aux);
			numberItems--;
		}
	}
	count = numberItems;
	fclose(fileDescriptor);
	return true;
}

bool CpuTargetTable::forceReadFileAddressEth(const char *fileName)	{
	/* Here we read the original file as usual */
	FILE *fileDescriptor;
	bool validAddress;
	uint64_t numberItems,i;
	size_t r;
	uint8_t rawvalue[50];
	char aux[100],*hextemp;
	fileDescriptor = fopen(fileName,"r");
	if(fileDescriptor == NULL)	{
		fprintf(stderr,"[E] Error opening the file %s, line %i\n",fileName,__LINE__ - 2);
		return false;
	}
	/*Count lines in the file*/
	numberItems = 0;
	while(!feof(fileDescriptor))	{
		hextemp = fgets(aux,100,fileDescriptor);
		trim(aux," \t\n\r");
		if(hextemp == aux)	{
			r = strlen(aux);
			if(r >= 40)	{
				numberItems++;
			}
		}
	}
	fseek(fileDescriptor,0,SEEK_SET);

	match_bytes = 20;		/*20 bytes beacuase we only need the data in binary*/
	count = numberItems;

	printf("[+] Allocating memory for %" PRIu64 " elements: %.2f MB\n",numberItems,(double)(((double) sizeof(struct address_value)*numberItems)/(double)1048576));
	entries = (struct address_value*) malloc(sizeof(struct address_value)*numberItems);
	checkpointer((void *)entries,__FILE__,"malloc","addressTable" ,__LINE__ -1 );


	if(!initBloomFilter(&filter,count,config.bloom_multiplier)) {
		fclose(fileDescriptor);
		return false;
	}

	i = 0;
	while(i < numberItems)	{
		validAddress = false;
		memset(aux,0,100);
		memset(entries[i].value,0,sizeof(struct address_value));
		hextemp = fgets(aux,100,fileDescriptor);
		trim(aux," \t\n\r");
		r = strlen(aux);
		if(r >= 40 && r <= 42){
			switch(r)		{
				case 40:
					if(isValidHex(aux)){
						hexs2bin(aux,rawvalue);
						bloom_add(&filter, rawvalue ,sizeof(struct address_value));
						memcpy(entries[i].value,rawvalue,sizeof(struct address_value));
						i++;
						validAddress = true;
					}
				break;
				case 42:
					if(isValidHex(aux+2)){
						hexs2bin(aux+2,rawvalue);
						bloom_add(&filter, rawvalue ,sizeof(struct address_value));
						memcpy(entries[i].value,rawvalue,sizeof(struct address_value));
						i++;
						validAddress = true;
					}
				break;
			}
		}
		if(!validAddress)	{
			fprintf(stderr,"[I] Ommiting invalid line %s\n",aux);
			numberItems--;
		}
	}

	fclose(fileDescriptor);
	return true;
}



bool CpuTargetTable::forceReadFileXPoint(const char *fileName)	{
	/* Here we read the original file as usual */
	FILE *fileDescriptor;
	uint64_t numberItems,i;
	size_t r,lenaux;
	uint8_t rawvalue[100];
	char aux[1000],*hextemp;
	Tokenizer tokenizer_xpoint;	//tokenizer
	fileDescriptor = fopen(fileName,"r");
	if(fileDescriptor == NULL)	{
		fprintf(stderr,"[E] Error opening the file %s, line %i\n",fileName,__LINE__ - 2);
		return false;
	}
	/*Count lines in the file*/
	numberItems = 0;
	while(!feof(fileDescriptor))	{
		hextemp = fgets(aux,1000,fileDescriptor);
		trim(aux," \t\n\r");
		if(hextemp == aux)	{
			r = strlen(aux);
			if(r >= 40)	{
				numberItems++;
			}
		}
	}
	fseek(fileDescriptor,0,SEEK_SET);

	match_bytes = 20;		/*20 bytes beacuase we only need the data in binary*/

	printf("[+] Allocating memory for %" PRIu64 " elements: %.2f MB\n",numberItems,(double)(((double) sizeof(struct address_value)*numberItems)/(double)1048576));
	entries = (struct address_value*) malloc(sizeof(struct address_value)*numberItems);
	checkpointer((void *)entries,__FILE__,"malloc","addressTable" ,__LINE__ - 1);

	count = numberItems;

	if(!initBloomFilter(&filter,count,config.bloom_multiplier)) {
		fclose(fileDescriptor);
		return false;
	}

	i= 0;
	while(i < count)	{
		memset(aux,0,1000);
		hextemp = fgets(aux,1000,fileDescriptor);
		memset((void *)&entries[i],0,sizeof(struct address_value));
		if(hextemp == aux)	{
			trim(aux," \t\n\r");
			stringtokenizer(aux,&tokenizer_xpoint);
			hextemp = nextToken(&tokenizer_xpoint);
			lenaux = strlen(hextemp);
			if(isValidHex(hextemp)) {
				switch(lenaux)	{
					case 64:	/*X value*/
						r = hexs2bin(aux,(uint8_t*) rawvalue);
						if(r)	{
							memcpy(entries[i].value,rawvalue,20);
							bloom_add(&filter,rawvalue,match_bytes);
						}
						else	{
							fprintf(stderr,"[E] error hexs2bin\n");
						}
					break;
					case 66:	/*Compress publickey*/
						r = hexs2bin(aux+2, (uint8_t*)rawvalue);
						if(r)	{
							memcpy(entries[i].value,rawvalue,20);
							bloom_add(&filter,rawvalue,match_bytes);
						}
						else	{
							fprintf(stderr,"[E] error hexs2bin\n");
						}
					break;
					case 130:	/* Uncompress publickey length*/
						r = hexs2bin(aux, (uint8_t*) rawvalue);
						if(r)	{
								memcpy(entries[i].value,rawvalue+2,20);
								bloom_add(&filter,rawvalue,match_bytes);
						}
						else	{
							fprintf(stderr,"[E] error hexs2bin\n");
						}
					break;
					default:
						fprintf(stderr,"[E] Omiting line unknow length size %li: %s\n",lenaux,aux);
					break;
				}
			}
			else	{
				fprintf(stderr,"[E] Ignoring invalid hexvalue %s\n",aux);
			}
			freetokenizer(&tokenizer_xpoint);
		}
		else	{
			fprintf(stderr,"[E] Omiting line : %s\n",aux);
			count--;
		}
		i++;
	}
	fclose(fileDescriptor);
	return true;
}


/*
	I write this as a function because i have the same segment of code in 3 different functions
*/

void CpuTargetTable::writeFileIfNeeded(const char *fileName)	{
	//printf("[D] FLAGSAVEREADFILE %i, FLAGREADEDFILE1 %i\n",FLAGSAVEREADFILE,FLAGREADEDFILE1);
	if(config.cache_targets && !cache_loaded)	{
		FILE *fileDescriptor;
		char fileBloomName[30];
		uint8_t checksum[32],hexPrefix[9];
		char dataChecksum[32],bloomChecksum[32];
		size_t bytesWrite;
		uint64_t dataSize;
		if(!sha256_file((const char*)fileName,checksum)){
			fprintf(stderr,"[E] sha256_file error line %i\n",__LINE__ - 1);
			exit(EXIT_FAILURE);
		}
		tohex_dst((char*)checksum,4,(char*)hexPrefix); // we save the prefix (last fourt bytes) hexadecimal value
		snprintf(fileBloomName,30,"data_%s.dat",hexPrefix);
		fileDescriptor = fopen(fileBloomName,"wb");
		dataSize = count * (sizeof(struct address_value));
		printf("[D] size data %li\n",dataSize);
		if(fileDescriptor != NULL)	{
			printf("[+] Writing file %s ",fileBloomName);


			//calculate filter checksum
			//write filter checksum (expected value to be checked)
			//write bloom filter structure
			//write bloom filter data


			//calculate dataChecksum
			//write data checksum (expected value to be checked)
			//write data size
			//write data




			sha256((uint8_t*)filter.bf,filter.bytes,(uint8_t*)bloomChecksum);
			printf(".");
			bytesWrite = fwrite(bloomChecksum,1,32,fileDescriptor);
			if(bytesWrite != 32)	{
				fprintf(stderr,"[E] Errore writing file, code line %i\n",__LINE__ - 2);
				exit(EXIT_FAILURE);
			}
			printf(".");

			bytesWrite = fwrite(&filter,1,sizeof(struct bloom),fileDescriptor);
			if(bytesWrite != sizeof(struct bloom))	{
				fprintf(stderr,"[E] Error writing file, code line %i\n",__LINE__ - 2);
				exit(EXIT_FAILURE);
			}
			printf(".");

			bytesWrite = fwrite(filter.bf,1,filter.bytes,fileDescriptor);
			if(bytesWrite != filter.bytes)	{
				fprintf(stderr,"[E] Error writing file, code line %i\n",__LINE__ - 2);
				fclose(fileDescriptor);
				exit(EXIT_FAILURE);
			}
			printf(".");

			/*
			if(FLAGDEBUG)	{
				hextemp = tohex((char*)filter.bf,32);
				printf("\n[D] first 32 bytes filter : %s\n",hextemp);
				bloom_print(&filter);
				free(hextemp);
			}
			*/



			sha256((uint8_t*)entries,dataSize,(uint8_t*)dataChecksum);
			printf(".");

			bytesWrite = fwrite(dataChecksum,1,32,fileDescriptor);
			if(bytesWrite != 32)	{
				fprintf(stderr,"[E] Errore writing file, code line %i\n",__LINE__ - 2);
				exit(EXIT_FAILURE);
			}
			printf(".");

			bytesWrite = fwrite(&dataSize,1,sizeof(uint64_t),fileDescriptor);
			if(bytesWrite != sizeof(uint64_t))	{
				fprintf(stderr,"[E] Errore writing file, code line %i\n",__LINE__ - 2);
				exit(EXIT_FAILURE);
			}
			printf(".");

			bytesWrite = fwrite(entries,1,dataSize,fileDescriptor);
			if(bytesWrite != dataSize)	{
				fprintf(stderr,"[E] Error writing file, code line %i\n",__LINE__ - 2);
				exit(EXIT_FAILURE);
			}
			printf(".");

			cache_loaded = 1;
			fclose(fileDescriptor);
			printf("\n");
		}
	}
}

BsgsTargets loadBsgsTargets(const char* fileName, Secp256K1& curve) {
    BsgsTargets result;
    Secp256K1* secp = &curve;
    FILE* fd;
    char* aux;
    char* aux2;
    Tokenizer tokenizerbsgs;
    uint64_t N = 0, i;
    uint32_t point_count;
		printf("[+] Opening file %s\n",fileName);
		fd = fopen(fileName,"rb");
		if(fd == NULL)	{
			fprintf(stderr,"[E] Can't open file %s\n",fileName);
			exit(EXIT_FAILURE);
		}
		aux = (char*) malloc(1024);
		checkpointer((void *)aux,__FILE__,"malloc","aux" ,__LINE__ - 1);
		while(!feof(fd))	{
			if(fgets(aux,1022,fd) == aux)	{
				trim(aux," \t\n\r");
				if(strlen(aux) >= 128)	{	//Length of a full address in hexadecimal without 04
						N++;
				}else	{
					if(strlen(aux) >= 66)	{
						N++;
					}
				}
			}
		}
		if(N == 0)	{
			fprintf(stderr,"[E] There is no valid data in the file\n");
			exit(EXIT_FAILURE);
		}
		result.points.resize(N);
		result.compressed = std::make_unique<bool[]>(N);




		fseek(fd,0,SEEK_SET);
		i = 0;
		while(!feof(fd))	{
			if(fgets(aux,1022,fd) == aux)	{
				trim(aux," \t\n\r");
				if(strlen(aux) >= 66)	{
					stringtokenizer(aux,&tokenizerbsgs);
					aux2 = nextToken(&tokenizerbsgs);


					switch(strlen(aux2))	{
						case 66:	//Compress

							if(secp->ParsePublicKeyHex(aux2,result.points[i],result.compressed[i]))	{
								i++;
							}
							else	{
								N--;
							}

						break;
						case 130:	//With the 04

							if(secp->ParsePublicKeyHex(aux2,result.points[i],result.compressed[i]))	{
								i++;
							}
							else	{
								N--;
							}

						break;
						default:
							printf("Invalid length: %s\n",aux2);
							N--;
						break;
					}
					freetokenizer(&tokenizerbsgs);
				}
			}
		}
		fclose(fd);
		free(aux);
		result.points.resize(N);
		point_count = N;
		if(point_count > 0)	{
			printf("[+] Added %u points from file\n",point_count);
		}
		else	{
			fprintf(stderr,"[E] The file don't have any valid publickeys\n");
			exit(EXIT_FAILURE);
		}
    return result;
}

bool readVanityTargets(const char* fileName, int existing_targets, int (*add_target)(char*)) {
	FILE *fileDescriptor;
	int len;
	char aux[100],*hextemp;

	fileDescriptor = fopen(fileName,"r");
	if(fileDescriptor == NULL)	{
		if(existing_targets == 0)	{
			fprintf(stderr,"[E] There aren't any vanity targets\n");
			return false;
		}
	}
	else	{
		while(!feof(fileDescriptor))	{
			hextemp = fgets(aux,100,fileDescriptor);
			if(hextemp == aux)	{
				trim(aux," \t\n\r");
				len = strlen(aux);
				if(len > 0 && len < 36){
					if(isValidBase58String(aux))	{
						add_target(aux);
					}
					else	{
						fprintf(stderr,"[E] the string \"%s\" is not valid Base58, omiting it\n",aux);
					}
				}
			}
		}
		fclose(fileDescriptor);
	}

    return true;
}

} // namespace keyhunt::core
