## Assignment 7A: Apache Spark (In-Class)

In this assignment you have to process a collection of JSON files (created from news websites) to do word counts.  A zip file containing multiple input files can be downloaded from Moodle. Each input file is a JSON file, which has several attributes; the ones we are interested in for Assignment 7A and 7B are these:
   *source_name, date_published, article_body*

1. First unzip the input files into a directory
2. Follow the instructions for setting up Spark on Eclipse given on Moodle.
3. Run the word count program which we have provided on the input files treated as text files, to make sure your Eclipse setup is proper.
4. Next, run both the news month count programs which find how many articles were published in each month
5. Today's main assignment is to modify the news month count program to find the **word count** ***in the article_body attribute*** by source; the output would be triples of the form (source_name, word, count). You can either concatenate strings to get the groups, or create rows and use the `.groupby()` function to group on multiple attributes.

**Sample data is given in the zip file on the moodle page.**

**Submission guidelines:** Submit your `.java` file(s) along with a README (in case you want to specify anything about your assignment) as a single tar.gz or zip file. The zip file should be named by your roll number, *e.g.* 160050001.zip