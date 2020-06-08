Instructions for setting up your project:
==========================================
The java/ directory contains servlets and related code. Copy all files in the java/ directory into your project's Java Resources -> src directory.

The jsCss/ directory contains the required javascript/css files for your web application. Copy files from the jsCss directory into your WebContent directory.

The jars/ directory contains the dependency jars for your project. Place them as follows:
* Right click on your project -> Build Path -> Configure Build Path -> Libraries
* Expand Classpath
* Choose Add External JARs on the right pane
* Add jsoup-1.11.3.jar, jackson-annotations-2.9.0.jar, jackson-core-2.9.6.jar, jackson-databind-2.9.6.jar
* Apply and Close

Now add required jars to your Tomcat Server classpath, as follows:
* Right click on your project -> Run As -> Run Configuratons
* Select your server "Tomcat v9.0 at localhost" from left pane
* Select Classpath tab -> User Entries
* Add jsoup-1.11.3.jar, jackson-annotations-2.9.0.jar, jackson-core-2.9.6.jar, jackson-databind-2.9.6.jar, postgresql-42.2.4.jar
* Click Apply -> Close

The sql file create.sql contains the DDL/DML statements to set up the database and sample data. You may need to create a database "whatasap"
before using this script.