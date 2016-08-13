#Qmery Storage (and stream) server
forked from kaltura vod module.
refer to [this](https://github.com/kaltura/nginx-vod-module) link for documentation

---
##Installation

###Webserver (required)
Download the latest version of nginx from [here](http://nginx.org/en/download.html) and exract it.
`$ tar -xzvf nginx-XX.tar.gz ./`

###VOD module (required)
Download and extract this library.

###Webdav module (Optional)
Download nginx Webdav module from [here](https://github.com/arut/nginx-dav-ext-module). optionally you can install your own webdav (or ftp) server.

`libexpat-dev` is required for this package

###Secure token module (Optional)
Download this module from [here](https://github.com/kaltura/nginx-secure-token-module).You can skip this step if you don't need encrypted urls.

###Compiling
after extracting the packages go ahead and make the new nginx service with
```
./configure \
--user=nginx                          \
--group=nginx                         \
--prefix=/etc/nginx                   \
--sbin-path=/usr/sbin/nginx           \
--conf-path=/etc/nginx/nginx.conf     \
--pid-path=/var/run/nginx.pid         \
--lock-path=/var/run/nginx.lock       \
--error-log-path=/var/log/nginx/error.log \
--http-log-path=/var/log/nginx/access.log \
--with-http_gzip_static_module        \
--with-http_stub_status_module        \
--with-file-aio                       \
--with-pcre	\
--with-http_realip_module             \
--with-http_ssl_module                \
--with-http_dav_module	\
--without-http_scgi_module            \
--without-http_uwsgi_module           \
--with-http_mp4_module \
--add-module=/PATH/TO/nginx-vod-module-custom	\
--add-module=/PATH/TO/nginx-dav-ext-module-master \
--add-module=/PATH/TO/nginx-secure-token-module
```

###Configuring
You can use the below sample and add this `server` block to your nginx
```
    server {
        listen       80;
        listen  443 ssl;

        ssl_certificate /PATH/TO/storage.crt;
        ssl_certificate_key /PATH/TO/storage.key;

        server_name  localhost storage.qmery.com;
        access_log  /var/log/nginx/access.log;
        root /PATH/TO/ROOT;
        #need to serve files with ajax requests
        add_header Access-Control-Allow-Origin *;

        location ~\.qmr {
			#rule to rewrite qmr files to mp4
            mp4;
            rewrite /repository/(.*)\.qmr $1.mp4;
        }
        location /repository {
        	#create /repository link to data root directory in order for old videos to work
            alias /PATH/TO/ROOT;
            index index.html;
        }
        location / {
            root   /PATH/TO/ROOT;
            index  index.html index.htm;
        }
        #these lines are to minimize the VOD module process
        open_file_cache          max=1000 inactive=5m;
        open_file_cache_valid    0;
        open_file_cache_min_uses 1;
        open_file_cache_errors   on;
        aio on;

        #These locations are needed to handle VTT urls
        location ~ ^/vtt/(\d+)/(.*)\.jpg$ {
            root /PATH/TO/ROOT;
            rewrite /vtt/(\d+)/(.*) /$1/$2 break;
            add_header Content-Type image/jpg;
            add_header Access-Control-Allow-Origin *;
        }
		location ~ ^/vtt/(\d+)/(.*)$ {
            root /PATH/TO/ROOT;
            rewrite /vtt/(\d+)/(.*) /$1/$2 break;
            secure_token_encrypt_uri on;
            secure_token_encrypt_uri_key YOUR_ENCRYPT_KEY_GOES_HERE;
            secure_token_encrypt_uri_iv YOUR_IV_GOES_HERE;
            secure_token_encrypt_uri_part $2;
            secure_token_types image/png image/jpg;
            add_header Access-Control-Allow-Origin *;
		    add_header Content-Type image/jpg;
        }

		#Locations to handle encrypted urls with file siffix, if you don't need suffix you can just ignore this 2 locations
		location ~ ^/pl/(.*)/(image.jpg)$ {
            root /PATH/TO/ROOT;
            rewrite /pl/(.*)/(image.jpg) /$1 break;
            secure_token_encrypt_uri on;
            secure_token_encrypt_uri_key YOUR_ENCRYPT_KEY_GOES_HERE;
            secure_token_encrypt_uri_iv YOUR_IV_GOES_HERE;
            secure_token_encrypt_uri_part $1;
            secure_token_types image/png image/jpg;
            add_header Content-Type image/jpg;
            expires 100d;
        }

		location ~ ^/pl/(.*)/(video.mp4)$ {
            root /PATH/TO/ROOT;
            rewrite /pl/(.*)/(video.mp4) /$1 break;
            secure_token_encrypt_uri on;
            secure_token_encrypt_uri_key YOUR_ENCRYPT_KEY_GOES_HERE;
            secure_token_encrypt_uri_iv YOUR_IV_GOES_HERE;
            secure_token_encrypt_uri_part $1;
            secure_token_types video/mp4;
            add_header Content-Type video/mp4;
            expires 100d;
        }

		#Main location to handle encrypted urls
		location ~ ^/pl/(.*) {
            root /PATH/TO/ROOT;
            rewrite /pl/(.*) /$1 break;
            secure_token_encrypt_uri on;
            secure_token_encrypt_uri_key YOUR_ENCRYPT_KEY_GOES_HERE;
            secure_token_encrypt_uri_iv YOUR_IV_GOES_HERE;
            secure_token_encrypt_uri_part $1;
            expires 100d;
        }

        #Main location to handle unencrypted HLS requests
        location ~ ^/qhls/(.*)$ {
            root /PATH/TO/ROOT;
            rewrite /qhls/(.*) /$1 break;
            vod hls;
            vod_mode local;
            vod_moov_cache moov_cache 512m;
            vod_hls_absolute_master_urls off;
            vod_hls_absolute_index_urls off;
            server_tokens off;
            add_header 'Access-Control-Allow-Origin' '*';
            break;
		}

		#HLS location to handle encrypted HLS urls
		location ~ ^/hls/(.*)/(.*)$ {
            root /PATH/TO/ROOT;
            rewrite /hls/(.*)/(.*) /$1/$2 break;
            vod hls;
            vod_secret_key '0x0000000000000000000001517E4AB836';
            vod_hls_encryption_method aes-128;
            vod_mode local;
            vod_moov_cache moov_cache 512m;
            vod_hls_absolute_master_urls off;
            vod_hls_absolute_index_urls off;

            secure_token_encrypt_uri on;
            secure_token_encrypt_uri_key YOUR_ENCRYPT_KEY_GOES_HERE;
            secure_token_encrypt_uri_iv YOUR_IV_GOES_HERE;
            secure_token_encrypt_uri_part $1;
            secure_token_types *;

            server_tokens off;
            add_header 'Access-Control-Allow-Origin' '*';
            break;
		}
        # deny access to .htaccess files, if Apache's document root
        # concurs with nginx's one
        #
        location ~ /\.ht {
            deny  all;
        }
}
#We are done.
#Below is configuration for Webdav server
server {
        listen 8080;
        client_max_body_size 5000M;
        location / {
                autoindex on;
                dav_methods PUT DELETE MKCOL COPY MOVE;
                dav_ext_methods PROPFIND OPTIONS;
                dav_access group:rw all:r;
                root /PATH/TO/ROOT;
                auth_basic "Restricted";
                auth_basic_user_file /PATH/TO/.htpasswd;
        }
}
```
And we're done.

Always be sure to run `nginx -t` to check your configuration. and we are ready to use the new installed server.
