## Assignment 7B: Apache Spark (Take-Home)

In this assignment you have to process a collection of JSON files to do sentiment analysis.  The inputs are

1. A zip with JSON files, provided for part 7A containing news articles.
2. A list of entity names, in a file, one name per line.
3. Positive sentiment word and negative sentiment word lists in two files.

Your task is to do the following:

1. Find occurrences of entity in body; for our purpose an exact case-insensitive match between a word in the body attribute and an entity name is considered a match.
2. Check for nearby sentiment words, where nearby means $\left| \text{position-of-entity} - \text{position-of-sentiment-word} \right| \le 5$
3. We consider the sentiment associated with an entity occurrence as positive (+1) if a positive sentiment word is nearby, negative (-1) if a negative sentiment word is nearby; an entity occurrence could be tagged as both positive and negative. If neither, we consider the sentiment as neutral (0).
4. Your goal is to find the aggregate sentiment per entity group by (source_name, year_month, entity, sentiment_type) as follows:
   1. NOTE: you are not allowed to use Spark SQL for this assignment
   2. Read the sentiment word files and the entity names file into Java data structure such as `HashSet<String>`. Each line of the input files has a single word. The data structures will be automatically shared with all copies of your program when it is run in parallel.

   3. Create `dataset<row>` with fields
      (source_name, year_month, entity, sentiment) where sentiment can be -1, 0 or +1

   4. Then get count group by (source_name, year_month, entity, sentiment) -- raw sentiment count
       output:  (source_name, year_month, entity, sentiment, count)

   5. from above find for each source, year_month, entity:
      - Overall sentiment: sum (sentiment*count)
      - Overall support:   sum(count).

   6. Now filter out all supports < 5

   7. Output to a file (source_name, year_month, entity, overall_sentiment) tuples in decreasing order of absolute value of overall_sentiment



**NOTE:** `org.apache.spark.sql.functions` contains aggregate functions that may be useful for this assignment. You can import this class and use aggregate methods from it by specifying the aggregates in the `agg()` function. For *e.g.* if you have a dataset ds1, with attributes a, b, and c, to get a result grouped by attribute 'a', with aggregates sum on attribute 'b' and average on 'c' per group

```java
ds1.groupBy("a").agg(functions.sum("b"),functions.avg("c"));
```


But if you want only a single aggregate, it's simpler to use

```java
ds1.groupBy("a").sum("b");
```

You can also use `ds1.col("a");` instead of directly giving the column name.

Documentation of functions class is available at https://spark.apache.org/docs/latest/api/java/org/apache/spark/sql/functions.html



**Sample data is given in the zip file on the moodle page.  Expected output will be shared later.**

**Submission guidelines:** Submit your `.java` file(s) along with a README (in case you want to specify anything about your assignment) as a single tar.gz or zip file.

**Late submission policy**: you lose 10% for each day or fraction thereof that you are late.  No submissions will be accepted after 10 PM on Sep 7, 2018.

Some useful links:

- https://spark.apache.org/docs/latest/sql-programming-guide.html
- For creating Dataframes:
  - https://spark.apache.org/docs/latest/sql-programming-guide.html#creating-dataframes
  - https://spark.apache.org/docs/latest/sql-programming-guide.html#untyped-dataset-operations-aka-dataframe-operations

