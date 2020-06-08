
//Set appropriate package name

import java.io.File;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Iterator;
import java.util.List;
import java.util.Scanner;
import java.util.Set;

import org.apache.spark.api.java.function.FlatMapFunction;
import org.apache.spark.sql.Dataset;
import org.apache.spark.sql.Row;
import org.apache.spark.sql.RowFactory;
import org.apache.spark.sql.SparkSession;
import org.apache.spark.sql.functions;
import org.apache.spark.sql.catalyst.encoders.ExpressionEncoder;
import org.apache.spark.sql.catalyst.encoders.RowEncoder;
import org.apache.spark.sql.types.DataTypes;
import org.apache.spark.sql.types.StructType;



/**
 * This class uses Dataset APIs of spark to count number of articles per month
 * The year-month is obtained as a dataset of String
 * */

public class EntitySentiment {

	public static void main(String[] args) throws FileNotFoundException {
		String otherFilesPath = "E:\\db\\";
		
		//Input dir - should contain all input json files
		String inputPath="E:\\db\\newsdata"; //Use absolute paths 
		
		//Ouput dir - this directory will be created by spark. Delete this directory between each run
		String outputPath="E:\\db\\output";   //Use absolute paths
		
		StructType structType = new StructType();
		structType = structType.add("source_name", DataTypes.StringType, false);
	    structType = structType.add("year_month", DataTypes.StringType, false); // false => not nullable
	    structType = structType.add("entity", DataTypes.StringType, false);
	    structType = structType.add("sentiment", DataTypes.IntegerType, false);
	    ExpressionEncoder<Row> myRowEncoder = RowEncoder.apply(structType);
		
		SparkSession sparkSession = SparkSession.builder()
				.appName("Month wise news articles")		//Name of application
				.master("local")								//Run the application on local node
				.config("spark.sql.shuffle.partitions","1")		//Number of partitions
				.getOrCreate();


		Scanner file = new Scanner(new File(otherFilesPath + "entities.txt"));
		Set<String> entities = new HashSet<>();
		while (file.hasNext()) {
			entities.add(file.next().trim());
		}
		file.close();

		file = new Scanner(new File(otherFilesPath + "positive-words.txt"));
		Set<String> positive_words = new HashSet<>();
		while (file.hasNext()) {
			positive_words.add(file.next().trim());
		}
		file.close();

		file = new Scanner(new File(otherFilesPath + "negative-words.txt"));
		Set<String> negative_words = new HashSet<>();
		while (file.hasNext()) {
			negative_words.add(file.next().trim());
		}
		file.close();

		
		//Read multi-line JSON from input files to dataset
		Dataset<Row> inputDataset=sparkSession.read().option("multiLine", true).json(inputPath);   
		
		
		// Apply the map function to extract the year-month
		Dataset<Row> yearMonthDataset=inputDataset.flatMap(new FlatMapFunction<Row,Row>(){
			/**
			 * 
			 */
			private static final long serialVersionUID = 1L;

			public Iterator<Row> call(Row row) throws Exception {
				String source = (String) row.getAs("source_name");
				// The first 7 characters of date_published gives the year-month 
				String yearMonthPublished=((String)row.getAs("date_published")).substring(0, 7);
				String line = (String) row.getAs("article_body");
				line = line.toLowerCase().replaceAll("[^A-Za-z]", " ");  //Remove all punctuation and convert to lower case
	    		line = line.replaceAll("( )+", " ");   //Remove all double spaces
	    		line = line.trim();
	    		String[] words = line.split(" ");
	    		
	    		List<Row> rowList = new ArrayList<>();
	    		for (int i=0; i<words.length; i++) {
	    			if (entities.contains(words[i])) {
	    				int start = i-5, end = i+5;
	    				if (start < 0) start = 0;
	    				if (end > words.length-1) end = words.length-1;

	    				int found = 0;
	    				for (int j=start; j<=end; j++) {
	    					if (positive_words.contains(words[j])) {
	    						found = 1;
	    						Row newRow = RowFactory.create(source, yearMonthPublished, words[i], 1);
	    						rowList.add(newRow);
	    					}
	    					if (negative_words.contains(words[j])) {
	    						found = 1;
	    						Row newRow = RowFactory.create(source, yearMonthPublished, words[i], -1);
	    						rowList.add(newRow);
	    					}
	    				}
    					if (found == 0) {
    						Row newRow = RowFactory.create(source, yearMonthPublished, words[i], 0);
    						rowList.add(newRow);
    					}
    					
	    			}
	    		}

				return rowList.iterator();	  
			}
			
		}, myRowEncoder);
		

		Dataset<Row> raw_sentiment_count = yearMonthDataset.groupBy("source_name", "year_month", "entity", "sentiment").count().as("count");  
		Dataset<Row> overall_sentiment = raw_sentiment_count.groupBy("source_name", "year_month", "entity").agg(functions.sum(raw_sentiment_count.col("sentiment").$times(raw_sentiment_count.col("count"))).as("overall_sentiment"), functions.sum("count").as("support"));
		Dataset<Row> filtered_overall_sentiment = overall_sentiment.filter(overall_sentiment.col("support").gt(4));
		Dataset<Row> final_result = filtered_overall_sentiment.drop("support").orderBy(filtered_overall_sentiment.col("overall_sentiment").desc());
		
		//Ouputs the result to a file
		final_result.toJavaRDD().saveAsTextFile(outputPath);	
		
	}
	
}
