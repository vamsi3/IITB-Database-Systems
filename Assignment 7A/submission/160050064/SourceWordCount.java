
//Set appropriate package name

import java.util.Arrays;
import java.util.Iterator;
import java.util.List;

import org.apache.spark.api.java.function.FlatMapFunction;
import org.apache.spark.api.java.function.MapFunction;
import org.apache.spark.sql.Dataset;
import org.apache.spark.sql.Encoders;
import org.apache.spark.sql.Row;
import org.apache.spark.sql.SparkSession;
import org.apache.spark.sql.catalyst.encoders.ExpressionEncoder;
import org.apache.spark.sql.catalyst.encoders.RowEncoder;
import org.apache.spark.sql.types.DataTypes;
import org.apache.spark.sql.types.StructType;



/**
 * This class uses Dataset APIs of spark to count number of articles per month
 * The year-month is obtained as a dataset of String
 * */

public class SourceWordCount {

	public static void main(String[] args) {
		
		//Input dir - should contain all input json files
		String inputPath="/users/ug16/vamsikrishna/Downloads/newsdata"; //Use absolute paths 
		
		//Ouput dir - this directory will be created by spark. Delete this directory between each run
		String outputPath="/users/ug16/vamsikrishna/output";   //Use absolute paths
		
		SparkSession sparkSession = SparkSession.builder()
				.appName("Month wise news articles")		//Name of application
				.master("local")								//Run the application on local node
				.config("spark.sql.shuffle.partitions","2")		//Number of partitions
				.getOrCreate();
		
		//Read multi-line JSON from input files to dataset
		Dataset<Row> inputDataset=sparkSession.read().option("multiLine", true).json(inputPath);   
		
		
		// Apply the map function to extract the year-month
		// Dataset<String> yearMonthDataset=inputDataset.map(new MapFunction<Row,String>(){
		// 	public String call(Row row) throws Exception {
		// 		// The first 7 characters of date_published gives the year-month 
		// 		String sourceName = ((String)row.getAs("source_name"));
		// 		String articleBody
		// 		return yearMonthPublished;
		// 	}
			
		// }, Encoders.STRING());

		Dataset<String> sourceWordsDataset = inputDataset.flatMap(
			new FlatMapFunction<Row, String>(){
				private static final long serialVersionUID = 1L;

				public Iterator<String> call(Row row) throws Exception {
			    	String source_name = ((String)row.getAs("source_name"));
			    	String line = ((String)row.getAs("article_body"));
					line = line.toLowerCase().replaceAll("[^A-Za-z]", " ");  //Remove all punctuation and convert to lower case
					line = line.replaceAll("( )+", " ");   //Remove all double spaces
					line = line.trim(); 
					List<String> wordList = Arrays.asList(line.split(" ")); //Get words
					wordList.replaceAll(word -> source_name + "," + word);
					return wordList.iterator();
				}
			}, Encoders.STRING());
		
		//Alternative way use flatmap using lambda function
		/*Dataset<String> yearMonthDataset=inputDataset.map(
				row-> {return ((String)row.getAs("date_published")).substring(0, 7);}, 
						Encoders.STRING());
		*/
		
		
		//The column name of the dataset for string encoding is value
		// Group by the desired column(s) and take count. groupBy() takes 1 or more parameters
		//Rename the result column as year_month_count
		Dataset<Row> count=sourceWordsDataset.groupBy("value").count().as("count");
		
		
		//Outputs the dataset to the standard output
		//count.show();
		
		
		//Ouputs the result to a file
		count.toJavaRDD().saveAsTextFile(outputPath);	
		
	}
	
}
