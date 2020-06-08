
//Set appropriate package name

import java.util.Arrays;
import java.util.Iterator;
import java.util.List;

import org.apache.spark.api.java.JavaPairRDD;
import org.apache.spark.api.java.JavaRDD;
import org.apache.spark.api.java.function.FlatMapFunction;
import org.apache.spark.api.java.function.Function2;
import org.apache.spark.sql.SparkSession;

import scala.Tuple2;



/**
 * This class uses RDDs to obtain word count for each word; json files are treated as text file
 * The year-month is obtained as a dataset of String
 * */


public class WordCount {

public static void main(String[] args) {
		
		//Input dir - should contain all input json files
		String inputPath="/users/ug16/<your_name>/Downloads/newsdata"; //Use absolute paths 
		
		//Ouput dir - this directory will be created by spark. Delete this directory between each run
		String outputPath="/users/ug16/<user_name>/output";   //Use absolute paths
		
		
		SparkSession sparkSession = SparkSession.builder()
				.appName("Word count")		//Name of application
				.master("local")								//Run the application on local node
				.config("spark.sql.shuffle.partitions","2")		//Number of partitions
				.getOrCreate();
		
		//Read lines from input file and create a JavaRDD
		JavaRDD<String> lines = sparkSession.read().textFile(inputPath).javaRDD();

		
		//Apply the flatMap function  
	    JavaRDD<String> words = lines.flatMap(
	    			new FlatMapFunction<String, String>(){
	    				public Iterator<String> call(String line) throws Exception {
	    					line = line.toLowerCase().replaceAll("[^A-Za-z]", " ");  //Remove all punctuation and convert to lower case
	    					line = line.replaceAll("( )+", " ");   //Remove all double spaces
	    					line = line.trim(); 
	    					List<String> wordList = Arrays.asList(line.split(" ")); //Get words
	    					return wordList.iterator();
	    				}
	    				
	    			}    );

		//Emit a <Word, 1> tuple for each word
	    JavaPairRDD<String, Integer> ones = words.mapToPair(s -> new Tuple2<>(s, 1));
	    
	    //Aggregate the count into a JavaPairRdd
	    JavaPairRDD<String,Integer> wordCount = ones.reduceByKey(new Function2<Integer,Integer,Integer>(){
			public Integer call(Integer i1, Integer i2) throws Exception {
				return i1+i2;
			}
	    	
	    });
	    
	    //Alternate way to run reduceByKey using lambda function
	    //JavaPairRDD<String, Integer> wordCount = ones.reduceByKey((i1, i2) -> i1 + i2);
	    
	    
	    //Save the output file
	    wordCount.saveAsTextFile(outputPath);
		
	}
	
}
