#include"stb_image.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>


class load_texture_image{

	public :
		unsigned char *data;
		unsigned int texture;
		int width, height, nrChannels;
		const char* image;
		
		load_texture_image(const char* path) : image(path) {
	 		
	 		glActiveTexture(GL_TEXTURE0);
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D, texture);
			// set the texture wrapping/filtering options (on currently bound texture)
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			
			// load and generate the texture
			load(path);		
		}
		
		void load(const char* image){
			data = stbi_load(image, &width, &height, &nrChannels, 0);
			generate(data);
		}
		
		void freeBuffer(){
			stbi_image_free(data);
		}
		
	private :
		
		void generate(unsigned char *data){
		
		
			if (data){
		
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB,
				GL_UNSIGNED_BYTE, data);
				glGenerateMipmap(GL_TEXTURE_2D);
		
			}
			else{std::cout << "Failed to load texture" << std::endl;}
		}
		
};
